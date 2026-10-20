// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_compute_smoke.h"
#include "tlg_native_device_resources.h"
#include "tlg_native_vk_draw.h"
#include "video_core/host_shaders/native_storage_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <vk_mem_alloc.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

namespace TlgNativeVk {
namespace {
constexpr std::uint32_t SmokeWords = 4;
constexpr VkDeviceSize SmokeBytes = SmokeWords * sizeof(std::uint32_t);
constexpr std::uint64_t SmokeComputeId = 0xfffffffffffd1001ull;
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
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool initialized = false;
    bool attempted = false;
};

ComputeSmoke::ComputeSmoke(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                           DeviceResources& resources, ResourceRegistry& registry)
    : state_{std::make_unique<State>(instance, scheduler, resources, registry)} {}

ComputeSmoke::~ComputeSmoke() {
    if (state_->buffer == VK_NULL_HANDLE) return;
    // Shader and storage buffer are not released until deferred recording has
    // reached a completed Vulkan submission; only the opt-in smoke waits here.
    state_->scheduler.EndRendering();
    state_->scheduler.Finish();
    if (state_->pipeline_created) {
        state_->device_resources.RetireComputePipeline(SmokeComputeId);
        state_->device_resources.CollectRetiredAfterIdle();
    }
    vmaDestroyBuffer(state_->instance.GetAllocator(), state_->buffer,
                     state_->allocation);
}

bool ComputeSmoke::Init() {
    if (state_->initialized) return true;
    if (state_->buffer != VK_NULL_HANDLE) return false;
    if (state_->instance.MaxPushDescriptors() < 1) {
        std::puts("Native Vulkan compute smoke: push descriptors unsupported");
        return false;
    }
    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = SmokeBytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VmaAllocationInfo alloc_info{};
    if (vmaCreateBuffer(state_->instance.GetAllocator(), &ci, &alloc_ci,
                        &state_->buffer, &state_->allocation, &alloc_info) != VK_SUCCESS)
        return false;
    state_->mapped = alloc_info.pMappedData;
    if (!state_->mapped) return false;
    std::memset(state_->mapped, 0xa5, SmokeBytes);
    if (vmaFlushAllocation(state_->instance.GetAllocator(),
                           state_->allocation, 0, SmokeBytes) != VK_SUCCESS)
        return false;

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

    const auto pipeline = state_->pipeline;
    const auto layout = state_->layout;
    const vk::Buffer gpu_buffer{state_->buffer};
    state_->scheduler.Record([pipeline,layout,gpu_buffer](vk::CommandBuffer cmd) {
        const vk::DescriptorBufferInfo info{
            .buffer = gpu_buffer,
            .offset = 0,
            .range = SmokeBytes,
        };
        const vk::WriteDescriptorSet write{
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &info,
        };
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                 layout, 0, std::span{&write, std::size_t{1}});
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
    if (vmaInvalidateAllocation(state_->instance.GetAllocator(),
                                state_->allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
        return false;
    const auto* words = static_cast<const std::uint32_t*>(state_->mapped);
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
