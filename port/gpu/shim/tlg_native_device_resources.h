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
#include <span>

namespace Vulkan { class Instance; class Scheduler; }
namespace Shader { struct RuntimeInfo; struct Profile; }

namespace TlgNativeVk {
struct GcnShaderInput;
struct NativeShaderKey;
class NativeShaderRegistry;
class DeviceResources {
public:
    DeviceResources(const Vulkan::Instance& instance, ResourceRegistry& registry);
    ~DeviceResources();
    DeviceResources(const DeviceResources&) = delete;
    DeviceResources& operator=(const DeviceResources&) = delete;

    // The only immediately drawable kind currently implemented: host-staged
    // GPU index data with an explicit 16/32-bit format. No guest pointer is
    // retained; initial_data must have exactly index_count * index_width bytes.
    // Independent native Vulkan pipeline from already-translated SPIR-V.
    // Initial implementation supports a fixed full-screen triangle vertex
    // layout and one color attachment. Not yet a general game material path.
    bool CreateGraphicsPipeline(std::uint64_t guest_id,
                                std::span<const std::uint32_t> vertex_spirv,
                                std::span<const std::uint32_t> fragment_spirv,
                                std::uint32_t color_format);
    // Native game shader path: consume *verified high-level Js* GCN shader
    // inputs directly, without reading Gnm/PM4 register packets. Fails closed
    // unless the shader/resource specialization is complete and both stages
    // translate to resource-free SPIR-V suitable for the current pipeline.
    bool CreateResourceFreeGraphicsPipelineFromGCN(
        std::uint64_t guest_id, const GcnShaderInput& vertex,
        const Shader::RuntimeInfo& vertex_runtime,
        const GcnShaderInput& fragment,
        const Shader::RuntimeInfo& fragment_runtime,
        const Shader::Profile& profile, std::uint32_t color_format);
    // Fast native shader path: reuse immutable, already translated SPIR-V
    // entries for the exact game, driver, compiler and descriptor specialization.
    // This avoids re-running GCN translation when the pipeline is recreated.
    bool CreateGraphicsPipelineFromCachedSpirv(
        std::uint64_t guest_id, const NativeShaderRegistry& shader_cache,
        const NativeShaderKey& vertex_key,
        const NativeShaderKey& fragment_key, std::uint32_t color_format);
    void RetireGraphicsPipeline(std::uint64_t guest_id);
    // GPU-owner-thread-only lookup. A caller must not keep the returned handle
    // beyond replacement, retirement or a GPU submission lifetime.
    bool QueryGraphicsPipeline(std::uint64_t guest_id,
                               std::uint64_t& vk_handle) const;

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

    struct ColorTargetInfo {
        std::uint64_t image = 0, view = 0;
        std::uint32_t width = 0, height = 0, format = 0;
    };
    // Native Vulkan GPU-thread-only handle lookup. These handles remain owned
    // by DeviceResources; callers must not destroy or retain them after retire.
    bool QueryColorTarget(std::uint64_t guest_id, ColorTargetInfo& out) const;

    // Record native Vulkan synchronization (same GPU scheduler/queue as the
    // game's presentation path), independent of PS4 PM4 packet decoding.
    // The caller may acknowledge resource readiness only AFTER all required
    // barriers are recorded in the Vulkan submission order.
    bool RecordIndexReadBarrier(std::uint64_t guest_id, Vulkan::Scheduler& scheduler);
    bool RecordColorAttachmentTransition(std::uint64_t guest_id,
                                         Vulkan::Scheduler& scheduler);

    // Physically destroy retired resources only after the Vulkan device is
    // idle. This method may block; do NOT call from a page-fault handler.
    void CollectRetiredAfterIdle();

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace TlgNativeVk
