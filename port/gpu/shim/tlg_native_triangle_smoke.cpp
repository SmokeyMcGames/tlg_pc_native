// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_triangle_smoke.h"
#include "tlg_native_device_resources.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/native_triangle_frag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
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
    bool initialized = false;
    bool emitted = false;
};

TriangleSmoke::TriangleSmoke(const Vulkan::Instance& instance,
                              Vulkan::Scheduler& scheduler, DeviceResources& resources)
    : state_{std::make_unique<State>(instance, scheduler, resources)} {}
TriangleSmoke::~TriangleSmoke() {
    if (!state_->initialized) return;
    // Keep the pipeline and target alive until submitted native draws finish.
    (void)state_->instance.GetDevice().waitIdle();
    state_->pipeline.reset();
    state_->layout.reset();
    state_->resources.RetireColorTarget(SmokeTargetId);
    state_->resources.CollectRetiredAfterIdle();
}

bool TriangleSmoke::Init() {
    if (state_->initialized) return true;
    if (!state_->resources.CreateColorTarget(SmokeTargetId, SmokeSize.width,
                                               SmokeSize.height,
                                               VK_FORMAT_R8G8B8A8_UNORM))
        return false;
    DeviceResources::ColorTargetInfo target{};
    if (!state_->resources.QueryColorTarget(SmokeTargetId, target)) return false;
    VkImageView raw_view = VK_NULL_HANDLE;
    static_assert(sizeof(raw_view) == sizeof(target.view));
    std::memcpy(&raw_view, &target.view, sizeof(raw_view));
    state_->color_view = vk::ImageView{raw_view};

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
    if (!state_->resources.RecordColorAttachmentTransition(SmokeTargetId, state_->scheduler))
        return false;
    state_->scheduler.EndRendering();
    const auto pipeline = *state_->pipeline;
    const auto color = state_->color_view;
    state_->scheduler.Record([pipeline,color](vk::CommandBuffer cmd) {
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
    });
    state_->emitted = true;
    std::puts("Native Vulkan triangle: offscreen PM4-independent draw recorded");
    return true;
}
} // namespace TlgNativeVk
