// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Native Vulkan index-buffer ownership built directly on the port's existing
// VkDevice and Vulkan Memory Allocator, without any PM4/GCN register dependency.
//
// This does not create guest textures or shader pipelines yet. GPU operations
// remain opt-in until resource and framebuffer parity are established.
#include "tlg_native_vk_resources.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Vulkan { class Instance; }

namespace TlgNativeVk {
class DeviceResources {
public:
    DeviceResources(const Vulkan::Instance& instance, ResourceRegistry& registry);
    ~DeviceResources();
    DeviceResources(const DeviceResources&) = delete;
    DeviceResources& operator=(const DeviceResources&) = delete;

    // The only immediately drawable kind currently implemented: host-staged
    // GPU index data with an explicit 16/32-bit format. No guest pointer is
    // retained; initial_data must have exactly index_count * index_width bytes.
    bool CreateIndexBuffer(std::uint64_t guest_id, std::uint64_t index_count,
                           std::uint32_t index_width, const void* initial_data,
                           std::size_t initial_bytes);
    bool UpdateIndexBuffer(std::uint64_t guest_id, std::uint64_t first_index,
                           std::uint32_t index_width, const void* source,
                           std::size_t source_bytes, std::uint64_t upload_epoch);
    void RetireIndexBuffer(std::uint64_t guest_id);

    // Native Vulkan offscreen 2D color target. The backend is responsible for
    // issuing an actual VkImageMemoryBarrier2 to COLOR_ATTACHMENT_OPTIMAL before
    // the registry is allowed to acknowledge barriers and use this target.
    bool CreateColorTarget(std::uint64_t guest_id, std::uint32_t width,
                           std::uint32_t height, std::uint32_t vk_format);
    void RetireColorTarget(std::uint64_t guest_id);

    // Physically destroy retired resources only after the Vulkan device is
    // idle. This method may block; do NOT call from a page-fault handler.
    void CollectRetiredAfterIdle();

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace TlgNativeVk
