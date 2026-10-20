// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Native Vulkan index-buffer ownership built directly on the port's existing
// VkDevice and Vulkan Memory Allocator, without any PM4/GCN register dependency.
//
// This does not create guest textures or shader pipelines yet. GPU operations
// remain opt-in until resource and framebuffer parity are established.
#include "tlg_native_vk_resources.h"
#include "tlg_native_descriptor_layout.h"
#include "tlg_native_descriptor_bindings.h"
#include "tlg_native_texture_layout.h"
#include "tlg_native_rgba8_detile.h"
#include "tlg_native_rgba8_macro_detile.h"
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
    bool CreateGraphicsPipeline(
        std::uint64_t guest_id,
        std::span<const std::uint32_t> vertex_spirv,
        std::span<const std::uint32_t> fragment_spirv,
        std::uint32_t color_format,
        std::span<const NativeDescriptorBinding> resource_bindings = {});
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
    // Translate and link explicitly verified storage-buffer/image/sampler
    // layouts without using PM4. This creates a Vulkan pipeline only; game
    // rendering additionally requires live descriptors, push constants,
    // verified vertex fetch, barriers and render-target ownership.
    bool CreateVerifiedGraphicsPipelineFromGCN(
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
    bool QueryGraphicsPipelineLayout(std::uint64_t guest_id,
                                     std::uint64_t& vk_layout) const;

    // Dedicated, descriptor-free native Vulkan compute pipeline. Compute
    // handles are type-tagged and cannot pass graphics draw validation.
    bool CreateComputePipeline(
        std::uint64_t guest_id, std::span<const std::uint32_t> compute_spirv,
        std::span<const NativeDescriptorBinding> resource_bindings = {});
    bool CreateResourceFreeComputePipelineFromGCN(
        std::uint64_t guest_id, const GcnShaderInput& compute,
        const Shader::RuntimeInfo& runtime, const Shader::Profile& profile);
    // Accept only a fully validated native descriptor-layout contract.
    // This does not submit the game's compute dispatch or memory barriers.
    bool CreateVerifiedComputePipelineFromGCN(
        std::uint64_t guest_id, const GcnShaderInput& compute,
        const Shader::RuntimeInfo& runtime, const Shader::Profile& profile);
    bool CreateComputePipelineFromCachedSpirv(
        std::uint64_t guest_id, const NativeShaderRegistry& shader_cache,
        const NativeShaderKey& compute_key);
    void RetireComputePipeline(std::uint64_t guest_id);
    bool QueryComputePipeline(std::uint64_t guest_id,
                              std::uint64_t& vk_handle) const;
    bool QueryComputePipelineLayout(std::uint64_t guest_id,
                                    std::uint64_t& vk_layout) const;

    // Independent, host-staged storage buffer allocation. Data is copied
    // immediately; no game memory is retained. A real host->shader barrier
    // must be recorded before it can appear in a native descriptor set.
    bool CreateStorageBuffer(std::uint64_t guest_id, const void* source,
                             std::uint64_t source_bytes);
    bool RecordStorageBufferHostWriteBarrier(std::uint64_t guest_id,
                                             Vulkan::Scheduler& scheduler);
    struct StorageBufferInfo {
        std::uint64_t vk_buffer = 0, capacity_bytes = 0, generation = 0;
    };
    bool QueryStorageBuffer(std::uint64_t guest_id, StorageBufferInfo& out) const;
    // Call only after this scheduler's GPU work and shader->host barrier have
    // completed. No invalidation or guest-address translation is attempted.
    bool ReadStorageBufferAfterGpuIdle(std::uint64_t guest_id, void* destination,
                                       std::uint64_t destination_bytes) const;
    void RetireStorageBuffer(std::uint64_t guest_id);

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

    // First genuinely native sampled-texture path. Accepted input must be
    // linear RGBA8 (not PS4 tiled), single mip and 2D. Staging memory stays
    // alive until the image upload and all dependent GPU draws have retired.
    bool CreateSampledTextureRGBA8(
        std::uint64_t guest_id, std::uint32_t width, std::uint32_t height,
        const void* source, std::uint64_t source_row_pitch,
        std::uint64_t source_available_bytes);
    // PS4 1D thin microtiled 32bpp RGBA8, verified surface pitch and
    // padded height only. Detile in a private CPU buffer before Vulkan
    // staging. Macro-tiled, swizzled banks, compressed images and mips are
    // deliberately not supported by this entry point.
    bool CreateSampledTexture1DTiledRgba8(
        std::uint64_t guest_id, std::uint32_t width, std::uint32_t height,
        std::uint32_t tiled_pitch, std::uint32_t tiled_height,
        Rgba8Tiling tile_mode, const void* source,
        std::uint64_t source_available_bytes);
    // Verified single-sample 2D thin macro-tiled RGBA8 only, GCN
    // P8_32x32_16x16 / 16 bank / non-alt layout. The caller must supply the
    // original bank swizzle from trusted Js image metadata. Does not detile
    // compressed, swizzled-bank variants, MSAA, depth or multiple mips.
    bool CreateSampledTexture2DMacroRgba8(
        std::uint64_t guest_id, std::uint32_t width, std::uint32_t height,
        std::uint32_t tiled_pitch, std::uint32_t tiled_height,
        std::uint32_t bank_swizzle, Rgba8MacroTiling tile_mode,
        const void* source, std::uint64_t source_available_bytes);
    bool RecordSampledTextureUpload(std::uint64_t guest_id,
                                    Vulkan::Scheduler& scheduler);
    void RetireSampledTexture(std::uint64_t guest_id);
    bool CreateNearestClampSampler(std::uint64_t guest_id);
    void RetireSampler(std::uint64_t guest_id);

    struct SampledImageInfo {
        std::uint64_t image_view = 0, sampler = 0;
        std::uint32_t width = 0, height = 0;
        std::uint64_t image_generation = 0, sampler_generation = 0;
    };
    bool QuerySampledBinding(std::uint64_t texture_id, std::uint64_t sampler_id,
                             SampledImageInfo& result) const;

    // The GPU submission owner records complete sampled-image/sampler/
    // storage-buffer descriptors on the Vulkan scheduler. Every resource
    // must match its current generation and reflected pipeline layout.
    // Storage images remain unsupported; no native game draw is enabled.
    bool RecordNativeDescriptors(
        std::uint64_t pipeline_id, NativePipelineKind pipeline_kind,
        std::span<const NativeGuestDescriptorBinding> assignments,
        Vulkan::Scheduler& scheduler);

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
