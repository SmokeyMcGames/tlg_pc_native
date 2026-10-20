// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_device_resources.h"
#include "video_core/renderer_vulkan/vk_instance.h"
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

std::uint64_t Identity(VkBuffer buffer) {
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
    std::mutex mutex;
    std::unordered_map<std::uint64_t, Buffer> active;
    std::vector<Buffer> retired;

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
}
} // namespace TlgNativeVk
