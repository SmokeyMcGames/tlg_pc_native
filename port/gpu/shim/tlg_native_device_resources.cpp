// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_device_resources.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace TlgNativeVk {
namespace {
constexpr std::uint64_t MaxNativeIndexBuffer = 256ull * 1024 * 1024; // 256 MiB

template<class Handle> std::uint64_t Identity(Handle buffer) {
    static_assert(sizeof(buffer) == sizeof(std::uint64_t));
    std::uint64_t value = 0;
    std::memcpy(&value, &buffer, sizeof(value));
    return value;
}
}

struct DeviceResources::State {
    explicit State(const Vulkan::Instance& src, ResourceRegistry& dst)
        : instance{src}, registry{dst}, allocator{src.GetAllocator()} {}
    const Vulkan::Instance& instance;
    ResourceRegistry& registry;
    VmaAllocator allocator;
    struct Buffer {
        VkBuffer gpu = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        std::uint64_t index_count = 0;
        std::uint32_t index_width = 0;
    };
    struct GraphicsPipeline {
        vk::UniquePipelineLayout layout{};
        vk::UniquePipeline pipeline{};
    };
    struct ColorTarget {
        VkImage gpu = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        vk::UniqueImageView view{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t format = 0;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::unique_ptr<GraphicsPipeline>> pipelines;
    std::vector<std::unique_ptr<GraphicsPipeline>> pipelines_retired;
    std::unordered_map<std::uint64_t, Buffer> active;
    std::vector<Buffer> retired;
    std::unordered_map<std::uint64_t, std::unique_ptr<ColorTarget>> color_active;
    std::vector<std::unique_ptr<ColorTarget>> color_retired;

    void Destroy(ColorTarget& image) {
        // VkImageView depends on VkImage; release the view first.
        image.view.reset();
        if (image.gpu != VK_NULL_HANDLE)
            vmaDestroyImage(allocator, image.gpu, image.allocation);
        image.gpu = VK_NULL_HANDLE;
        image.allocation = VK_NULL_HANDLE;
    }
    void Destroy(Buffer& b) {
        if (b.gpu != VK_NULL_HANDLE)
            vmaDestroyBuffer(allocator, b.gpu, b.allocation);
        b = {};
    }
};

DeviceResources::DeviceResources(const Vulkan::Instance& instance, ResourceRegistry& registry)
    : state_{std::make_unique<State>(instance, registry)} {}

DeviceResources::~DeviceResources() {
    // Initialization/teardown happens on the GPU-owning side.
    // Ensure submitted work cannot still reference any Vulkan allocation.
    CollectRetiredAfterIdle();
    std::scoped_lock lock{state_->mutex};
    for (auto& [id, buffer] : state_->active) {
        state_->registry.UnregisterIndexBuffer(id);
        state_->Destroy(buffer);
    }
    state_->active.clear();
    for (auto& [id, pipeline] : state_->pipelines) {
        state_->registry.UnregisterPipeline(id);
        pipeline.reset();
    }
    state_->pipelines.clear();
    for (auto& [id, target] : state_->color_active) {
        state_->registry.UnregisterTargetSet(id);
        state_->Destroy(*target);
    }
    state_->color_active.clear();
}

bool DeviceResources::CreateGraphicsPipeline(
    std::uint64_t guest, std::span<const std::uint32_t> vertex_spirv,
    std::span<const std::uint32_t> fragment_spirv, std::uint32_t color_format) {
    if (!guest || color_format == VK_FORMAT_UNDEFINED ||
        vertex_spirv.size() < 5 || fragment_spirv.size() < 5 ||
        vertex_spirv.size() > (1u << 20) || fragment_spirv.size() > (1u << 20) ||
        vertex_spirv[0] != 0x07230203 || fragment_spirv[0] != 0x07230203)
        return false;

    const auto device = state_->instance.GetDevice();
    auto [vs_result, vs] = device.createShaderModuleUnique({
        .codeSize = vertex_spirv.size_bytes(), .pCode = vertex_spirv.data(),
    });
    if (vs_result != vk::Result::eSuccess) return false;
    auto [fs_result, fs] = device.createShaderModuleUnique({
        .codeSize = fragment_spirv.size_bytes(), .pCode = fragment_spirv.data(),
    });
    if (fs_result != vk::Result::eSuccess) return false;

    auto resource = std::make_unique<State::GraphicsPipeline>();
    auto [layout_result, layout] =
        device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{});
    if (layout_result != vk::Result::eSuccess) return false;
    resource->layout = std::move(layout);

    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = *vs, .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = *fs, .pName = "main",
        },
    };
    const auto format = static_cast<vk::Format>(color_format);
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport{
        .viewportCount = 1, .scissorCount = 1,
    };
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo samples{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState color{
        .colorWriteMask = vk::ColorComponentFlagBits::eR |
                          vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB |
                          vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo blend{
        .attachmentCount = 1, .pAttachments = &color,
    };
    const std::array dynamic_states{
        vk::DynamicState::eViewport, vk::DynamicState::eScissor,
    };
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    auto [result, pipeline] = device.createGraphicsPipelineUnique(
        {}, vk::GraphicsPipelineCreateInfo{
            .pNext = &rendering,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input,
            .pViewportState = &viewport,
            .pRasterizationState = &raster,
            .pMultisampleState = &samples,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamic,
            .layout = *resource->layout,
        });
    if (result != vk::Result::eSuccess) return false;
    resource->pipeline = std::move(pipeline);
    const std::uint64_t handle = Identity(static_cast<VkPipeline>(*resource->pipeline));
    std::scoped_lock lock{state_->mutex};
    if (!state_->registry.RegisterPipeline(guest, handle)) return false;
    if (auto old = state_->pipelines.find(guest); old != state_->pipelines.end()) {
        state_->pipelines_retired.push_back(std::move(old->second));
        old->second = std::move(resource);
    } else {
        state_->pipelines.emplace(guest, std::move(resource));
    }
    return true;
}

bool DeviceResources::QueryGraphicsPipeline(std::uint64_t guest,
                                           std::uint64_t& handle) const {
    std::scoped_lock lock{state_->mutex};
    const auto it = state_->pipelines.find(guest);
    if (it == state_->pipelines.end() || !it->second->pipeline) return false;
    handle = Identity(static_cast<VkPipeline>(*it->second->pipeline));
    return true;
}

void DeviceResources::RetireGraphicsPipeline(std::uint64_t guest) {
    std::scoped_lock lock{state_->mutex};
    auto it = state_->pipelines.find(guest);
    if (it == state_->pipelines.end()) return;
    state_->registry.UnregisterPipeline(guest);
    state_->pipelines_retired.push_back(std::move(it->second));
    state_->pipelines.erase(it);
}

bool DeviceResources::CreateIndexBuffer(std::uint64_t id, std::uint64_t count,
                                        std::uint32_t width, const void* source,
                                        std::size_t nbytes) {
    if (!id || !source || (width != 2 && width != 4) ||
        !count || count > MaxNativeIndexBuffer / width ||
        nbytes != count * width) return false;
    const VkBufferCreateInfo bi{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = static_cast<VkDeviceSize>(nbytes),
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo ai{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer gpu = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(state_->allocator, &bi, &ai, &gpu,
                        &alloc, &allocation_info) != VK_SUCCESS)
        return false;
    if (!allocation_info.pMappedData) {
        vmaDestroyBuffer(state_->allocator, gpu, alloc);
        return false;
    }
    std::memcpy(allocation_info.pMappedData, source, nbytes);
    if (vmaFlushAllocation(state_->allocator, alloc, 0, nbytes) != VK_SUCCESS) {
        vmaDestroyBuffer(state_->allocator, gpu, alloc);
        return false;
    }
    const std::uint64_t handle = Identity(gpu);
    // Enforce one lock order (state mutex -> registry mutex) for all create,
    // update, and retire operations; concurrent guests cannot invert locks.
    std::scoped_lock lock{state_->mutex};
    if (!state_->registry.RegisterIndexBuffer(id, handle, count)) {
        vmaDestroyBuffer(state_->allocator, gpu, alloc);
        return false;
    }
    if (auto it = state_->active.find(id); it != state_->active.end()) {
        state_->retired.push_back(it->second);
        it->second = {gpu, alloc, count, width};
    } else {
        state_->active.emplace(id, State::Buffer{gpu, alloc, count, width});
    }
    return true;
}

bool DeviceResources::UpdateIndexBuffer(std::uint64_t id, std::uint64_t first,
                                        std::uint32_t width, const void* source,
                                        std::size_t nbytes, std::uint64_t epoch) {
    if (!id || !source || !nbytes || (width != 2 && width != 4) ||
        nbytes % width || !epoch) return false;

    std::scoped_lock lock{state_->mutex};
    const auto it = state_->active.find(id);
    if (it == state_->active.end() || it->second.index_width != width ||
        first > it->second.index_count ||
        nbytes / width > it->second.index_count - first) return false;

    // Copy-on-write is essential: the old allocation may be referenced by
    // Vulkan submissions that have not completed. Never mutate it in place.
    // This prototype copies the full host-visible buffer; a future native
    // streaming allocator should batch updates using per-frame staging rings.
    const std::size_t total = it->second.index_count * width;
    const VkBufferCreateInfo bi{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = total,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo ai{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VmaAllocationInfo previous{};
    vmaGetAllocationInfo(state_->allocator, it->second.allocation, &previous);
    if (!previous.pMappedData) return false;
    VkBuffer next_gpu = VK_NULL_HANDLE;
    VmaAllocation next_alloc = VK_NULL_HANDLE;
    VmaAllocationInfo next{};
    if (vmaCreateBuffer(state_->allocator, &bi, &ai, &next_gpu,
                        &next_alloc, &next) != VK_SUCCESS) return false;
    if (!next.pMappedData) {
        vmaDestroyBuffer(state_->allocator, next_gpu, next_alloc);
        return false;
    }

    std::memcpy(next.pMappedData, previous.pMappedData, total);
    std::memcpy(static_cast<unsigned char*>(next.pMappedData) + first * width,
                source, nbytes);
    if (vmaFlushAllocation(state_->allocator, next_alloc, 0, total) != VK_SUCCESS) {
        vmaDestroyBuffer(state_->allocator, next_gpu, next_alloc);
        return false;
    }

    // Publishing a new handle increments its resource generation and
    // invalidates previous bindings. The caller MUST bind the new handle and
    // record a HOST_WRITE -> INDEX_READ Vulkan barrier before a draw is valid.
    if (!state_->registry.RegisterIndexBuffer(id, Identity(next_gpu),
                                              it->second.index_count)) {
        vmaDestroyBuffer(state_->allocator, next_gpu, next_alloc);
        return false;
    }
    const auto old = it->second;
    it->second = {next_gpu, next_alloc, old.index_count, width};
    state_->retired.push_back(old);
    state_->registry.NoteBufferWrite(id, epoch);
    return true;
}

bool DeviceResources::CreateColorTarget(std::uint64_t guest,
                                        std::uint32_t width, std::uint32_t height,
                                        std::uint32_t format) {
    if (!guest || !width || !height || width > 8192 || height > 8192 ||
        format == VK_FORMAT_UNDEFINED)
        return false;
    const VkImageCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(format),
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation_ci{
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(state_->allocator, &ci, &allocation_ci,
                       &image, &allocation, nullptr) != VK_SUCCESS)
        return false;

    const vk::ImageViewCreateInfo view_ci{
        .image = vk::Image{image},
        .viewType = vk::ImageViewType::e2D,
        .format = static_cast<vk::Format>(format),
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto [result, view] = state_->instance.GetDevice().createImageViewUnique(view_ci);
    if (result != vk::Result::eSuccess || !view) {
        vmaDestroyImage(state_->allocator, image, allocation);
        return false;
    }
    const auto handle = Identity(static_cast<VkImageView>(*view));
    auto target = std::make_unique<State::ColorTarget>();
    target->gpu = image;
    target->allocation = allocation;
    target->view = std::move(view);
    target->width = width;
    target->height = height;
    target->format = format;
    std::scoped_lock lock{state_->mutex};
    if (!state_->registry.RegisterTargetSet(guest, handle)) {
        state_->Destroy(*target);
        return false;
    }
    if (auto it = state_->color_active.find(guest); it != state_->color_active.end()) {
        state_->color_retired.push_back(std::move(it->second));
        it->second = std::move(target);
    } else {
        state_->color_active.emplace(guest, std::move(target));
    }
    // The image starts UNDEFINED. No Vk render can use it until the GPU side
    // records a real layout transition and acknowledges that barrier.
    return true;
}

bool DeviceResources::QueryColorTarget(std::uint64_t guest,
                                       ColorTargetInfo& out) const {
    std::scoped_lock lock{state_->mutex};
    const auto it = state_->color_active.find(guest);
    if (it == state_->color_active.end() || !it->second->view)
        return false;
    const auto& target = *it->second;
    out = {
        .image = Identity(target.gpu),
        .view = Identity(static_cast<VkImageView>(*target.view)),
        .width = target.width,
        .height = target.height,
        .format = target.format,
    };
    return true;
}

bool DeviceResources::RecordIndexReadBarrier(std::uint64_t guest,
                                             Vulkan::Scheduler& scheduler) {
    VkBuffer handle = VK_NULL_HANDLE;
    std::uint64_t byte_size = 0;
    {
        std::scoped_lock lock{state_->mutex};
        auto it = state_->active.find(guest);
        if (it == state_->active.end()) return false;
        handle = it->second.gpu;
        byte_size = it->second.index_count * it->second.index_width;
    }
    const vk::BufferMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eIndexInput,
        .dstAccessMask = vk::AccessFlagBits2::eIndexRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = vk::Buffer{handle},
        .offset = 0,
        .size = byte_size,
    };
    // DependencyInfo must be created INSIDE the deferred closure because
    // its pBufferMemoryBarriers pointer would otherwise dangle.
    scheduler.Record([barrier](vk::CommandBuffer cmd) {
        const vk::DependencyInfo dependencies{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier,
        };
        cmd.pipelineBarrier2(dependencies);
    });
    return true;
}

bool DeviceResources::RecordColorAttachmentTransition(std::uint64_t guest,
                                                        Vulkan::Scheduler& scheduler) {
    vk::ImageMemoryBarrier2 barrier{};
    {
        std::scoped_lock lock{state_->mutex};
        auto it = state_->color_active.find(guest);
        if (it == state_->color_active.end()) return false;
        auto& image = *it->second;
        if (image.layout == vk::ImageLayout::eColorAttachmentOptimal)
            return true;
        if (image.layout != vk::ImageLayout::eUndefined)
            return false; // unknown transition path; reject rather than guess
        barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = vk::AccessFlags2{},
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                             vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vk::Image{image.gpu},
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        image.layout = vk::ImageLayout::eColorAttachmentOptimal;
    }
    scheduler.Record([barrier](vk::CommandBuffer cmd) {
        const vk::DependencyInfo dependencies{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };
        cmd.pipelineBarrier2(dependencies);
    });
    return true;
}

void DeviceResources::RetireColorTarget(std::uint64_t guest) {
    std::scoped_lock lock{state_->mutex};
    auto it = state_->color_active.find(guest);
    if (it == state_->color_active.end()) return;
    state_->registry.UnregisterTargetSet(guest);
    state_->color_retired.push_back(std::move(it->second));
    state_->color_active.erase(it);
}

void DeviceResources::RetireIndexBuffer(std::uint64_t id) {
    std::scoped_lock lock{state_->mutex};
    auto it = state_->active.find(id);
    if (it == state_->active.end()) return;
    state_->registry.UnregisterIndexBuffer(id);
    state_->retired.push_back(it->second);
    state_->active.erase(it);
}

void DeviceResources::CollectRetiredAfterIdle() {
    // Do not release guest-visible resources until all in-flight GPU work
    // has completed. Future versions should replace this with per-resource
    // completion tickets for nonblocking steady-state retirement.
    (void)state_->instance.GetDevice().waitIdle();
    std::scoped_lock lock{state_->mutex};
    for (auto& b : state_->retired) state_->Destroy(b);
    state_->retired.clear();
    for (auto& image : state_->color_retired) state_->Destroy(*image);
    state_->color_retired.clear();
    state_->pipelines_retired.clear();
}
} // namespace TlgNativeVk
