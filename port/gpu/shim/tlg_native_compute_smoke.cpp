// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_compute_smoke.h"
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
}
struct ComputeSmoke::State {
    explicit State(const Vulkan::Instance& inst, Vulkan::Scheduler& sched)
        : instance(inst), scheduler(sched) {}
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    vk::UniqueDescriptorSetLayout descriptors{};
    vk::UniquePipelineLayout layout{};
    vk::UniquePipeline pipeline{};
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool initialized = false;
    bool attempted = false;
};

ComputeSmoke::ComputeSmoke(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler)
    : state_{std::make_unique<State>(instance, scheduler)} {}

ComputeSmoke::~ComputeSmoke() {
    if (state_->buffer == VK_NULL_HANDLE) return;
    // Shader and storage buffer are not released until deferred recording has
    // reached a completed Vulkan submission; only the opt-in smoke waits here.
    state_->scheduler.EndRendering();
    state_->scheduler.Finish();
    state_->pipeline.reset();
    state_->layout.reset();
    state_->descriptors.reset();
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

    const auto device = state_->instance.GetDevice();
    const vk::DescriptorSetLayoutBinding storage_binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
    };
    const vk::DescriptorSetLayoutCreateInfo set_info{
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1,
        .pBindings = &storage_binding,
    };
    auto [set_result, desc] = device.createDescriptorSetLayoutUnique(set_info);
    if (set_result != vk::Result::eSuccess) return false;
    state_->descriptors = std::move(desc);
    const vk::DescriptorSetLayout set_layout = *state_->descriptors;
    const vk::PipelineLayoutCreateInfo layout_ci{
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_ci);
    if (layout_result != vk::Result::eSuccess) return false;
    state_->layout = std::move(layout);

    const auto shader = Vulkan::CompileSPV(NATIVE_STORAGE_COMP, device);
    if (!shader) return false;
    const vk::PipelineShaderStageCreateInfo stage{
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = shader,
        .pName = "main",
    };
    auto [pipeline_result, pipeline] = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{.stage = stage, .layout = *state_->layout});
    device.destroyShaderModule(shader);
    if (pipeline_result != vk::Result::eSuccess) return false;
    state_->pipeline = std::move(pipeline);
    state_->initialized = true;
    return true;
}

bool ComputeSmoke::RecordAndVerifyOnce() {
    if (!state_->initialized || state_->attempted) return false;
    state_->attempted = true;
    state_->scheduler.EndRendering(); // graphics pass must end before compute

    const auto pipeline = *state_->pipeline;
    const auto layout = *state_->layout;
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
