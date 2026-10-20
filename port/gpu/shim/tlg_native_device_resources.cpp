// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_device_resources.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>
#include <algorithm>
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
    for (auto& [id, target] : state_->color_active) {
        state_->registry.UnregisterTargetSet(id);
        state_->Destroy(*target);
    }
    state_->color_active.clear();
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
    if (!state_->registry.RegisterIndexBuffer(id, handle, count)) {
        vmaDestroyBuffer(state_->allocator, gpu, alloc);
        return false;
    }
    std::scoped_lock lock{state_->mutex};
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
    void* mapped = nullptr;
    const VkResult map = vmaMapMemory(state_->allocator, it->second.allocation, &mapped);
    if (map != VK_SUCCESS || !mapped) return false;
    std::memcpy(static_cast<unsigned char*>(mapped) + first * width, source, nbytes);
    const VkResult flush = vmaFlushAllocation(state_->allocator,
                                               it->second.allocation, first * width, nbytes);
    vmaUnmapMemory(state_->allocator, it->second.allocation);
    if (flush != VK_SUCCESS) return false;
    // Require the draw path to record a matching host-write -> index-read
    // VkBufferMemoryBarrier2 before acknowledging the upload epoch.
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
    if (!state_->registry.RegisterTargetSet(guest, handle)) {
        view.reset();
        vmaDestroyImage(state_->allocator, image, allocation);
        return false;
    }
    auto target = std::make_unique<State::ColorTarget>();
    target->gpu = image;
    target->allocation = allocation;
    target->view = std::move(view);
    target->width = width;
    target->height = height;
    target->format = format;
    std::scoped_lock lock{state_->mutex};
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
}
} // namespace TlgNativeVk
