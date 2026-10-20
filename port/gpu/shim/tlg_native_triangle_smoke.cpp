// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_triangle_smoke.h"
#include "tlg_native_device_resources.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/native_triangle_frag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <vk_mem_alloc.h>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace TlgNativeVk {
namespace {
constexpr std::uint64_t SmokeTargetId = 0xfffffffffffe1001ull;
constexpr vk::Extent2D SmokeSize{64, 64};
constexpr vk::Format SmokeFormat = vk::Format::eR8G8B8A8Unorm;
}
struct TriangleSmoke::State {
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    DeviceResources& resources;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::ImageView color_view{};
    VkImage color_image = VK_NULL_HANDLE;
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VmaAllocation readback_allocation = VK_NULL_HANDLE;
    void* readback_mapped = nullptr;
    bool target_created = false;
    bool initialized = false;
    bool emitted = false;
};

TriangleSmoke::TriangleSmoke(const Vulkan::Instance& instance,
                              Vulkan::Scheduler& scheduler, DeviceResources& resources)
    : state_{std::make_unique<State>(instance, scheduler, resources)} {}
TriangleSmoke::~TriangleSmoke() {
    if (!state_->target_created) return;
    // A deferred Scheduler::Record() closure may not have reached the GPU yet.
    // Finish recording and wait for execution before destroying pipeline/image.
    // This only happens for the opt-in native smoke at teardown.
    state_->scheduler.EndRendering();
    state_->scheduler.Finish();
    state_->pipeline.reset();
    state_->layout.reset();
    if (state_->readback_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(state_->instance.GetAllocator(), state_->readback_buffer,
                         state_->readback_allocation);
        state_->readback_buffer = VK_NULL_HANDLE;
        state_->readback_allocation = VK_NULL_HANDLE;
        state_->readback_mapped = nullptr;
    }
    state_->resources.RetireColorTarget(SmokeTargetId);
    state_->resources.CollectRetiredAfterIdle();
}

bool TriangleSmoke::Init() {
    if (state_->initialized) return true;
    if (!state_->resources.CreateColorTarget(SmokeTargetId, SmokeSize.width,
                                               SmokeSize.height,
                                               VK_FORMAT_R8G8B8A8_UNORM))
        return false;
    state_->target_created = true;
    DeviceResources::ColorTargetInfo target{};
    if (!state_->resources.QueryColorTarget(SmokeTargetId, target)) return false;
    VkImageView raw_view = VK_NULL_HANDLE;
    static_assert(sizeof(raw_view) == sizeof(target.view));
    std::memcpy(&raw_view, &target.view, sizeof(raw_view));
    state_->color_view = vk::ImageView{raw_view};
    static_assert(sizeof(state_->color_image) == sizeof(target.image));
    std::memcpy(&state_->color_image, &target.image, sizeof(state_->color_image));

    // Real GPU -> CPU verification: a host-visible readback buffer is filled
    // only via vkCmdCopyImageToBuffer after the native triangle has rendered.
    constexpr VkDeviceSize readback_size = SmokeSize.width * SmokeSize.height * 4u;
    const VkBufferCreateInfo bi{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = readback_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo ai{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(state_->instance.GetAllocator(), &bi, &ai,
                        &state_->readback_buffer, &state_->readback_allocation,
                        &allocation_info) != VK_SUCCESS)
        return false;
    state_->readback_mapped = allocation_info.pMappedData;
    if (!state_->readback_mapped) return false;
    // The readback buffer is deliberately initialized to a diagnostic pattern.
    std::memset(state_->readback_mapped, 0xa5, static_cast<std::size_t>(readback_size));
    if (vmaFlushAllocation(state_->instance.GetAllocator(),
                           state_->readback_allocation, 0, readback_size) != VK_SUCCESS)
        return false;

    const auto device = state_->instance.GetDevice();
    auto [layout_result, layout] = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{});
    if (layout_result != vk::Result::eSuccess) return false;
    state_->layout = std::move(layout);

    const vk::ShaderModule vertex = Vulkan::CompileSPV(FS_TRI_VERT, device);
    const vk::ShaderModule fragment = Vulkan::CompileSPV(NATIVE_TRIANGLE_FRAG, device);
    if (!vertex || !fragment) {
        if (vertex) device.destroyShaderModule(vertex);
        if (fragment) device.destroyShaderModule(fragment);
        return false;
    }
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vertex,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fragment,
            .pName = "main",
        },
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &SmokeFormat,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport{.viewportCount=1,.scissorCount=1};
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo samples{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState blend_attachment{
        .colorWriteMask = vk::ColorComponentFlagBits::eR |
                          vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB |
                          vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo blend{
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport,vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    auto [result, pipeline] = device.createGraphicsPipelineUnique(
        {}, vk::GraphicsPipelineCreateInfo{
            .pNext=&rendering, .stageCount=static_cast<std::uint32_t>(stages.size()),
            .pStages=stages.data(), .pVertexInputState=&vertex_input,
            .pInputAssemblyState=&assembly, .pViewportState=&viewport,
            .pRasterizationState=&raster, .pMultisampleState=&samples,
            .pColorBlendState=&blend, .pDynamicState=&dynamic,
            .layout=*state_->layout,
        });
    device.destroyShaderModule(vertex);
    device.destroyShaderModule(fragment);
    if (result != vk::Result::eSuccess) return false;
    state_->pipeline = std::move(pipeline);
    state_->initialized = true;
    return true;
}

bool TriangleSmoke::RecordOnce() {
    if (!state_->initialized || state_->emitted) return false;
    // Vulkan forbids changing an attachment image's layout while another
    // dynamic rendering instance is active. End the guest's current pass first.
    state_->scheduler.EndRendering();
    if (!state_->resources.RecordColorAttachmentTransition(SmokeTargetId, state_->scheduler))
        return false;
    const auto pipeline = *state_->pipeline;
    const auto color = state_->color_view;
    const auto native_image = vk::Image{state_->color_image};
    const auto staging = vk::Buffer{state_->readback_buffer};
    state_->scheduler.Record([pipeline,color,native_image,staging](vk::CommandBuffer cmd) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView=color,
            .imageLayout=vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp=vk::AttachmentLoadOp::eClear,
            .storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{
                .color = {.float32 = std::array<float,4>{0.0f,0.0f,0.0f,1.0f}},
            },
        };
        const vk::RenderingInfo pass{
            .renderArea={.extent=SmokeSize},
            .layerCount=1,
            .colorAttachmentCount=1,
            .pColorAttachments=&attachment,
        };
        cmd.beginRendering(pass);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,pipeline);
        cmd.setViewport(0, vk::Viewport{
            .width=float(SmokeSize.width),.height=float(SmokeSize.height),
            .minDepth=0.0f,.maxDepth=1.0f,
        });
        cmd.setScissor(0, vk::Rect2D{.extent=SmokeSize});
        cmd.draw(3,1,0,0);
        cmd.endRendering();

        // Native Vulkan Sync2 image transition: color-write -> copy source.
        const vk::ImageMemoryBarrier2 to_transfer{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = native_image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        const vk::DependencyInfo image_dependency{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_transfer,
        };
        cmd.pipelineBarrier2(image_dependency);

        const vk::BufferImageCopy copy{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {SmokeSize.width, SmokeSize.height, 1},
        };
        cmd.copyImageToBuffer(native_image, vk::ImageLayout::eTransferSrcOptimal,
                              staging, copy);

        // Native Vulkan Sync2 buffer barrier: transfer-write -> host-read.
        const vk::BufferMemoryBarrier2 to_cpu{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging,
            .offset = 0,
            .size = SmokeSize.width * SmokeSize.height * 4u,
        };
        const vk::DependencyInfo cpu_dependency{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &to_cpu,
        };
        cmd.pipelineBarrier2(cpu_dependency);
    });

    state_->emitted = true; // never accidentally issue the smoke twice
    // One-shot GPU readback is intentionally synchronous and opt-in. Submit
    // the command before inspecting the mapped staging allocation.
    state_->scheduler.Finish();
    if (vmaInvalidateAllocation(state_->instance.GetAllocator(),
                                state_->readback_allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS) {
        std::puts("Native Vulkan triangle: readback memory invalidation FAILED");
        return false;
    }
    const auto* rgba = static_cast<const std::uint8_t*>(state_->readback_mapped);
    const auto middle = (SmokeSize.height / 2u * SmokeSize.width +
                         SmokeSize.width / 2u) * 4u;
    const std::uint8_t r = rgba[middle], g = rgba[middle + 1];
    const std::uint8_t b = rgba[middle + 2], a = rgba[middle + 3];
    // fs_tri.vert supplies UV ~0.5 at the center; native_triangle.frag
    // outputs (uv.x, uv.y, 0.25, 1). Tolerance allows rasterization rounding.
    const bool pass = r >= 90 && r <= 165 && g >= 90 && g <= 165 &&
                      b >= 50 && b <= 85 && a >= 250;
    std::printf("Native Vulkan triangle GPU readback: %s "
                "(center RGBA %u,%u,%u,%u; expected roughly 128,128,64,255)\n",
                pass ? "PASS" : "FAIL", unsigned(r), unsigned(g),
                unsigned(b), unsigned(a));
    return pass;
}
} // namespace TlgNativeVk
