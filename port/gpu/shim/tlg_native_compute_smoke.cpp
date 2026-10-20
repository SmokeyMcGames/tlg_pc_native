// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_compute_smoke.h"
#include "tlg_native_device_resources.h"
#include "tlg_native_vk_draw.h"
#include "video_core/host_shaders/native_storage_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace TlgNativeVk {
namespace {
constexpr std::uint32_t SmokeWords = 4;
constexpr VkDeviceSize SmokeBytes = SmokeWords * sizeof(std::uint32_t);
constexpr std::uint64_t SmokeComputeId = 0xfffffffffffd1001ull;
constexpr std::uint64_t SmokeStorageId = 0xfffffffffffd1002ull;
}
struct ComputeSmoke::State {
    State(const Vulkan::Instance& inst, Vulkan::Scheduler& sched,
          DeviceResources& owner, ResourceRegistry& resources)
        : instance(inst), scheduler(sched), device_resources(owner),
          registry(resources) {}
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    DeviceResources& device_resources;
    ResourceRegistry& registry;
    vk::Pipeline pipeline{}; // borrowed, owned by DeviceResources
    vk::PipelineLayout layout{}; // borrowed, owned by DeviceResources
    bool pipeline_created = false;
    bool buffer_created = false;
    bool initialized = false;
    bool attempted = false;
};

ComputeSmoke::ComputeSmoke(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                           DeviceResources& resources, ResourceRegistry& registry)
    : state_{std::make_unique<State>(instance, scheduler, resources, registry)} {}

ComputeSmoke::~ComputeSmoke() {
    if (!state_->buffer_created && !state_->pipeline_created) return;
    // Drain deferred commands before retiring any native Vulkan allocation.
    state_->scheduler.EndRendering();
    state_->scheduler.Finish();
    if (state_->pipeline_created)
        state_->device_resources.RetireComputePipeline(SmokeComputeId);
    if (state_->buffer_created)
        state_->device_resources.RetireStorageBuffer(SmokeStorageId);
    state_->device_resources.CollectRetiredAfterIdle();
}

bool ComputeSmoke::Init() {
    if (state_->initialized) return true;
    if (state_->buffer_created) return false;
    if (state_->instance.MaxPushDescriptors() < 1) {
        std::puts("Native Vulkan compute smoke: push descriptors unsupported");
        return false;
    }
    // The native Vulkan owner stages bytes now and assigns a new identity.
    const std::array<std::uint8_t, SmokeBytes> source{
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
    };
    if (!state_->device_resources.CreateStorageBuffer(
            SmokeStorageId, source.data(), source.size()))
        return false;
    state_->buffer_created = true;

    // Create and register the REAL VkComputePipeline through the same
    // independent owner and type-safe ResourceRegistry used for future
    // native JsComputeContext dispatches.
    constexpr NativeDescriptorBinding resource_binding{
        .binding = 0,
        .descriptor_count = 1,
        .kind = NativeDescriptorKind::StorageBuffer,
    };
    const std::array bindings{resource_binding};
    if (!state_->device_resources.CreateComputePipeline(
            SmokeComputeId, NATIVE_STORAGE_COMP, bindings))
        return false;
    state_->pipeline_created = true;
    std::uint64_t pipeline_handle = 0, layout_handle = 0;
    std::uint64_t registered_handle = 0;
    if (!state_->device_resources.QueryComputePipeline(SmokeComputeId, pipeline_handle) ||
        !state_->device_resources.QueryComputePipelineLayout(SmokeComputeId, layout_handle) ||
        !state_->registry.LookupComputePipeline(SmokeComputeId, registered_handle) ||
        registered_handle != pipeline_handle)
        return false;
    VkPipeline raw_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout raw_layout = VK_NULL_HANDLE;
    static_assert(sizeof(raw_pipeline) == sizeof(pipeline_handle));
    static_assert(sizeof(raw_layout) == sizeof(layout_handle));
    std::memcpy(&raw_pipeline, &pipeline_handle, sizeof(raw_pipeline));
    std::memcpy(&raw_layout, &layout_handle, sizeof(raw_layout));
    state_->pipeline = vk::Pipeline{raw_pipeline};
    state_->layout = vk::PipelineLayout{raw_layout};
    state_->initialized = true;
    return true;
}

bool ComputeSmoke::RecordAndVerifyOnce() {
    if (!state_->initialized || state_->attempted) return false;
    state_->attempted = true;
    state_->scheduler.EndRendering(); // graphics pass must end before compute

    if (!state_->device_resources.RecordStorageBufferHostWriteBarrier(
            SmokeStorageId, state_->scheduler))
        return false;
    DeviceResources::StorageBufferInfo storage{};
    if (!state_->device_resources.QueryStorageBuffer(SmokeStorageId, storage) ||
        storage.capacity_bytes != SmokeBytes)
        return false;
    const std::array<NativeGuestDescriptorBinding, 1> bound{{
        NativeGuestDescriptorBinding{
            .binding = 0,
            .kind = NativeDescriptorKind::StorageBuffer,
            .resources = {{SmokeStorageId, storage.generation}},
        },
    }};
    if (!state_->device_resources.RecordNativeDescriptors(
            SmokeComputeId, NativePipelineKind::Compute,
            bound, state_->scheduler))
        return false;

    VkBuffer raw_buffer = VK_NULL_HANDLE;
    static_assert(sizeof(raw_buffer) == sizeof(storage.vk_buffer));
    std::memcpy(&raw_buffer, &storage.vk_buffer, sizeof(raw_buffer));
    const vk::Buffer gpu_buffer{raw_buffer};
    const auto pipeline = state_->pipeline;
    state_->scheduler.Record([pipeline,gpu_buffer](vk::CommandBuffer cmd) {
        TlgNativeVk::EmitComputeDispatch(cmd, vk::PipelineBindPoint::eCompute,
                                         pipeline, TlgNativeVk::Dispatch{1,1,1});
        const vk::BufferMemoryBarrier2 to_cpu{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = gpu_buffer,
            .offset = 0,
            .size = SmokeBytes,
        };
        const vk::DependencyInfo dependency{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &to_cpu,
        };
        cmd.pipelineBarrier2(dependency);
    });

    // Optional one-shot verification only; do not enable for normal gameplay
    // until the asynchronous native compute scheduling path is complete.
    state_->scheduler.Finish();
    std::array<std::uint32_t, SmokeWords> words{};
    if (!state_->device_resources.ReadStorageBufferAfterGpuIdle(
            SmokeStorageId, words.data(), sizeof(words)))
        return false;
    bool pass = true;
    for (unsigned i = 0; i < SmokeWords; ++i)
        pass = pass && (words[i] == 0x4e415400u + 7u * i);
    std::printf("Native Vulkan compute SSBO readback: %s "
                "(values %08x %08x %08x %08x)\n",
                pass ? "PASS" : "FAIL",
                words[0], words[1], words[2], words[3]);
    return pass;
}
} // namespace TlgNativeVk
