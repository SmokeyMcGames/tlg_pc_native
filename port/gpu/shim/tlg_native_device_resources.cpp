// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_device_resources.h"
#include "tlg_native_gcn_compiler.h"
#include "tlg_native_spirv_registry.h"
#include "shader_recompiler/resource.h"
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
#include <new>
#include <utility>

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
        vk::UniqueDescriptorSetLayout descriptors{};
        vk::UniquePipelineLayout layout{};
        vk::UniquePipeline pipeline{};
        std::vector<NativeDescriptorBinding> bindings{};
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
    struct SampledTexture {
        VkImage gpu = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        vk::UniqueImageView view{};
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation staging_allocation = VK_NULL_HANDLE;
        std::uint32_t width = 0, height = 0;
        NativeTextureTransition state{};
    };
    struct NativeSampler {
        vk::UniqueSampler handle{};
    };
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::unique_ptr<GraphicsPipeline>> pipelines;
    std::unordered_map<std::uint64_t, std::unique_ptr<GraphicsPipeline>> compute_pipelines;
    // Both types own a VkPipeline and a VkPipelineLayout, and can share
    // the same GPU-idle retirement queue without unsafe cross-stage reuse.
    std::vector<std::unique_ptr<GraphicsPipeline>> pipelines_retired;
    std::unordered_map<std::uint64_t, Buffer> active;
    std::vector<Buffer> retired;
    std::unordered_map<std::uint64_t, std::unique_ptr<ColorTarget>> color_active;
    std::vector<std::unique_ptr<ColorTarget>> color_retired;
    std::unordered_map<std::uint64_t, std::unique_ptr<SampledTexture>> sampled_textures;
    std::vector<std::unique_ptr<SampledTexture>> sampled_retired;
    std::unordered_map<std::uint64_t, std::unique_ptr<NativeSampler>> native_samplers;
    std::vector<std::unique_ptr<NativeSampler>> samplers_retired;

    void Destroy(SampledTexture& texture) {
        texture.state.Retire();
        texture.view.reset();
        if (texture.staging != VK_NULL_HANDLE)
            vmaDestroyBuffer(allocator, texture.staging, texture.staging_allocation);
        if (texture.gpu != VK_NULL_HANDLE)
            vmaDestroyImage(allocator, texture.gpu, texture.allocation);
        texture.staging = VK_NULL_HANDLE;
        texture.gpu = VK_NULL_HANDLE;
        texture.staging_allocation = VK_NULL_HANDLE;
        texture.allocation = VK_NULL_HANDLE;
    }
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
    for (auto& [id, pipeline] : state_->compute_pipelines) {
        state_->registry.UnregisterPipeline(id);
        pipeline.reset();
    }
    state_->compute_pipelines.clear();
    for (auto& [id, target] : state_->color_active) {
        state_->registry.UnregisterTargetSet(id);
        state_->Destroy(*target);
    }
    state_->color_active.clear();
    for (auto& [id, texture] : state_->sampled_textures) {
        state_->registry.UnregisterSampledImage(id);
        state_->Destroy(*texture);
    }
    state_->sampled_textures.clear();
    for (auto& [id, sampler] : state_->native_samplers)
        state_->registry.UnregisterSampler(id);
    state_->native_samplers.clear();
}

bool DeviceResources::CreateComputePipeline(
    std::uint64_t guest, std::span<const std::uint32_t> spirv,
    std::span<const NativeDescriptorBinding> resource_bindings) {
    if (!guest || spirv.size() < 5 || spirv.size() > (1u << 20) ||
        spirv[0] != 0x07230203u)
        return false;
    // The native stage resolver must supply a complete, verified descriptor
    // sequence. Never silently invent missing sampler/image bindings.
    std::vector<vk::DescriptorSetLayoutBinding> vk_bindings;
    std::uint32_t total_descriptors = 0;
    std::uint32_t next_binding = 0;
    vk_bindings.reserve(resource_bindings.size());
    for (std::size_t i = 0; i < resource_bindings.size(); ++i) {
        const auto& binding = resource_bindings[i];
        if (binding.binding != next_binding || binding.descriptor_count < 1 ||
            binding.descriptor_count > 64)
            return false;
        vk::DescriptorType type{};
        switch (binding.kind) {
        case NativeDescriptorKind::StorageBuffer:
            type = vk::DescriptorType::eStorageBuffer; break;
        case NativeDescriptorKind::SampledImage:
            type = vk::DescriptorType::eSampledImage; break;
        case NativeDescriptorKind::StorageImage:
            type = vk::DescriptorType::eStorageImage; break;
        case NativeDescriptorKind::Sampler:
            type = vk::DescriptorType::eSampler; break;
        default:
            return false;
        }
        if (binding.descriptor_count > state_->instance.MaxPushDescriptors() -
                                           total_descriptors)
            return false;
        total_descriptors += binding.descriptor_count;
        // Mirror the existing shader emitter: an image descriptor array of N
        // occupies N binding numbers, even though VkDescriptorSetLayoutBinding
        // stores that array as one layout entry.
        next_binding += binding.descriptor_count;
        vk_bindings.push_back({
            .binding = binding.binding,
            .descriptorType = type,
            .descriptorCount = binding.descriptor_count,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    const auto device = state_->instance.GetDevice();
    const vk::ShaderModuleCreateInfo module_ci{
        .codeSize = spirv.size_bytes(), .pCode = spirv.data(),
    };
    auto [module_result, module] = device.createShaderModuleUnique(module_ci);
    if (module_result != vk::Result::eSuccess)
        return false;
    auto resource = std::make_unique<State::GraphicsPipeline>();
    resource->bindings.assign(resource_bindings.begin(), resource_bindings.end());
    // GCN-generated compute code uses the canonical stage user-data push
    // structure. A high-level Js compute dispatch must provide *verified*
    // values before recording a native Vulkan dispatch.
    const vk::PushConstantRange push_constant{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };
    vk::DescriptorSetLayout set_handle{};
    if (!vk_bindings.empty()) {
        const vk::DescriptorSetLayoutCreateInfo desc_ci{
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<std::uint32_t>(vk_bindings.size()),
            .pBindings = vk_bindings.data(),
        };
        auto [descriptor_result, descriptors] =
            device.createDescriptorSetLayoutUnique(desc_ci);
        if (descriptor_result != vk::Result::eSuccess)
            return false;
        resource->descriptors = std::move(descriptors);
        set_handle = *resource->descriptors;
    }
    const vk::PipelineLayoutCreateInfo layout_ci{
        .setLayoutCount = set_handle ? 1u : 0u,
        .pSetLayouts = set_handle ? &set_handle : nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_ci);
    if (layout_result != vk::Result::eSuccess)
        return false;
    resource->layout = std::move(layout);
    const vk::PipelineShaderStageCreateInfo stage{
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = *module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo pipeline_ci{
        .stage = stage,
        .layout = *resource->layout,
    };
    auto [result, pipeline] = device.createComputePipelineUnique({}, pipeline_ci);
    if (result != vk::Result::eSuccess)
        return false;
    resource->pipeline = std::move(pipeline);
    const std::uint64_t handle = Identity(static_cast<VkPipeline>(*resource->pipeline));
    std::scoped_lock lock{state_->mutex};
    if (!state_->registry.RegisterComputePipeline(guest, handle))
        return false;
    if (auto it = state_->compute_pipelines.find(guest);
        it != state_->compute_pipelines.end()) {
        state_->pipelines_retired.push_back(std::move(it->second));
        it->second = std::move(resource);
    } else {
        state_->compute_pipelines.emplace(guest, std::move(resource));
    }
    return true;
}

bool DeviceResources::CreateResourceFreeComputePipelineFromGCN(
    std::uint64_t guest, const GcnShaderInput& compute,
    const Shader::RuntimeInfo& runtime, const Shader::Profile& profile) {
    if (compute.hw_stage != Shader::HwStage::Compute ||
        compute.sw_stage != Shader::SwStage::Compute)
        return false;
    auto translated = TranslateVerifiedGcn(compute, runtime, profile);
    if (translated.status != ShaderTranslationStatus::Success)
        return false;
    if (!translated.descriptor_layout.empty())
        return false;
    return CreateComputePipeline(guest, translated.spirv);
}

bool DeviceResources::CreateVerifiedComputePipelineFromGCN(
    std::uint64_t guest, const GcnShaderInput& compute,
    const Shader::RuntimeInfo& runtime, const Shader::Profile& profile) {
    if (!guest || compute.hw_stage != Shader::HwStage::Compute ||
        compute.sw_stage != Shader::SwStage::Compute)
        return false;
    auto compiled = TranslateVerifiedGcn(compute, runtime, profile);
    if (compiled.status != ShaderTranslationStatus::Success ||
        !ValidNativeDescriptorLayout(compiled.descriptor_layout))
        return false;
    return CreateComputePipeline(guest, compiled.spirv,
                                 compiled.descriptor_layout);
}

bool DeviceResources::CreateComputePipelineFromCachedSpirv(
    std::uint64_t guest, const NativeShaderRegistry& cache,
    const NativeShaderKey& key) {
    if (key.stage != NativeShaderStage::Compute)
        return false;
    const auto shader = cache.Find(key);
    if (!shader || shader->first_binding != 0 ||
        !ValidNativeDescriptorLayout(shader->descriptors))
        return false;
    return CreateComputePipeline(guest, shader->spirv, shader->descriptors);
}

bool DeviceResources::QueryComputePipelineLayout(
    std::uint64_t guest, std::uint64_t& handle) const {
    std::scoped_lock lock{state_->mutex};
    const auto it = state_->compute_pipelines.find(guest);
    if (it == state_->compute_pipelines.end() || !it->second->layout)
        return false;
    handle = Identity(static_cast<VkPipelineLayout>(*it->second->layout));
    return true;
}

void DeviceResources::RetireComputePipeline(std::uint64_t guest) {
    std::scoped_lock lock{state_->mutex};
    const auto it = state_->compute_pipelines.find(guest);
    if (it == state_->compute_pipelines.end())
        return;
    state_->registry.UnregisterPipeline(guest);
    state_->pipelines_retired.push_back(std::move(it->second));
    state_->compute_pipelines.erase(it);
}

bool DeviceResources::QueryComputePipeline(
    std::uint64_t guest, std::uint64_t& handle) const {
    std::scoped_lock lock{state_->mutex};
    const auto it = state_->compute_pipelines.find(guest);
    if (it == state_->compute_pipelines.end() || !it->second->pipeline)
        return false;
    handle = Identity(static_cast<VkPipeline>(*it->second->pipeline));
    return true;
}

bool DeviceResources::CreateGraphicsPipelineFromCachedSpirv(
    std::uint64_t guest, const NativeShaderRegistry& cache,
    const NativeShaderKey& vs_key, const NativeShaderKey& fs_key,
    std::uint32_t format) {
    if (!guest || vs_key.stage != NativeShaderStage::Vertex ||
        fs_key.stage != NativeShaderStage::Fragment ||
        vs_key.game_binary_hash != fs_key.game_binary_hash ||
        vs_key.compiler_revision != fs_key.compiler_revision ||
        vs_key.vk_vendor != fs_key.vk_vendor ||
        vs_key.vk_device != fs_key.vk_device ||
        vs_key.vk_driver != fs_key.vk_driver)
        return false;
    const auto vs = cache.Find(vs_key);
    const auto fs = cache.Find(fs_key);
    if (!vs || !fs)
        return false;
    std::vector<NativeDescriptorBinding> layout;
    if (!AssembleCachedGraphicsDescriptors(*vs, *fs, layout))
        return false;
    // Each shared_ptr keeps its SPIR-V and descriptor metadata immutable and
    // alive throughout Vulkan pipeline creation, even if the guest retires it.
    return CreateGraphicsPipeline(guest, vs->spirv, fs->spirv, format, layout);
}

bool DeviceResources::CreateResourceFreeGraphicsPipelineFromGCN(
    std::uint64_t guest, const GcnShaderInput& vertex,
    const Shader::RuntimeInfo& vertex_runtime,
    const GcnShaderInput& fragment,
    const Shader::RuntimeInfo& fragment_runtime,
    const Shader::Profile& profile, std::uint32_t format) {
    // This path is independent of the game's old PM4 shader selection.
    // It deliberately cannot bind shaders with unknown texture/buffer
    // descriptors to the native zero-descriptor Vulkan pipeline layout.
    if (!guest || !vertex.code_verified || !fragment.code_verified ||
        vertex.hw_stage != Shader::HwStage::Vertex ||
        vertex.sw_stage != Shader::SwStage::Vertex ||
        fragment.hw_stage != Shader::HwStage::Fragment ||
        fragment.sw_stage != Shader::SwStage::Fragment)
        return false;
    auto vs = TranslateVerifiedGcn(vertex, vertex_runtime, profile);
    if (vs.status != ShaderTranslationStatus::Success)
        return false;
    auto fs = TranslateVerifiedGcn(fragment, fragment_runtime, profile,
                                   vs.bindings);
    if (fs.status != ShaderTranslationStatus::Success ||
        !vs.descriptor_layout.empty() || !fs.descriptor_layout.empty())
        return false;
    return CreateGraphicsPipeline(guest, vs.spirv, fs.spirv, format);
}

bool DeviceResources::CreateVerifiedGraphicsPipelineFromGCN(
    std::uint64_t guest, const GcnShaderInput& vertex,
    const Shader::RuntimeInfo& vertex_runtime,
    const GcnShaderInput& fragment,
    const Shader::RuntimeInfo& fragment_runtime,
    const Shader::Profile& profile, std::uint32_t format) {
    if (!guest || vertex.hw_stage != Shader::HwStage::Vertex ||
        vertex.sw_stage != Shader::SwStage::Vertex ||
        fragment.hw_stage != Shader::HwStage::Fragment ||
        fragment.sw_stage != Shader::SwStage::Fragment)
        return false;
    auto vs = TranslateVerifiedGcn(vertex, vertex_runtime, profile);
    if (vs.status != ShaderTranslationStatus::Success)
        return false;
    auto fs = TranslateVerifiedGcn(fragment, fragment_runtime, profile,
                                   vs.bindings);
    if (fs.status != ShaderTranslationStatus::Success)
        return false;
    std::vector<NativeDescriptorBinding> layout = vs.descriptor_layout;
    layout.insert(layout.end(), fs.descriptor_layout.begin(),
                  fs.descriptor_layout.end());
    if (!ValidNativeDescriptorLayout(layout))
        return false;
    return CreateGraphicsPipeline(guest, vs.spirv, fs.spirv, format, layout);
}

bool DeviceResources::CreateGraphicsPipeline(
    std::uint64_t guest, std::span<const std::uint32_t> vertex_spirv,
    std::span<const std::uint32_t> fragment_spirv, std::uint32_t color_format,
    std::span<const NativeDescriptorBinding> resource_bindings) {
    if (!guest || color_format == VK_FORMAT_UNDEFINED ||
        vertex_spirv.size() < 5 || fragment_spirv.size() < 5 ||
        vertex_spirv.size() > (1u << 20) || fragment_spirv.size() > (1u << 20) ||
        vertex_spirv[0] != 0x07230203 || fragment_spirv[0] != 0x07230203)
        return false;

    // The descriptor layout is supplied by verified Js shader reflection,
    // not the PS4 GPU command register file. Any unsupported binding or
    // capacity overflow refuses pipeline creation.
    std::vector<vk::DescriptorSetLayoutBinding> layout_bindings;
    layout_bindings.reserve(resource_bindings.size());
    std::uint32_t next_binding = 0, descriptor_total = 0;
    const auto max_push = state_->instance.MaxPushDescriptors();
    for (const auto& description : resource_bindings) {
        if (description.binding != next_binding ||
            !description.descriptor_count || description.descriptor_count > 64 ||
            descriptor_total > max_push ||
            description.descriptor_count > max_push - descriptor_total)
            return false;
        vk::DescriptorType descriptor_type{};
        switch (description.kind) {
        case NativeDescriptorKind::StorageBuffer:
            descriptor_type = vk::DescriptorType::eStorageBuffer; break;
        case NativeDescriptorKind::SampledImage:
            descriptor_type = vk::DescriptorType::eSampledImage; break;
        case NativeDescriptorKind::StorageImage:
            descriptor_type = vk::DescriptorType::eStorageImage; break;
        case NativeDescriptorKind::Sampler:
            descriptor_type = vk::DescriptorType::eSampler; break;
        default:
            return false;
        }
        layout_bindings.push_back({
            .binding = description.binding,
            .descriptorType = descriptor_type,
            .descriptorCount = description.descriptor_count,
            .stageFlags = vk::ShaderStageFlagBits::eVertex |
                          vk::ShaderStageFlagBits::eFragment,
        });
        next_binding += description.descriptor_count;
        descriptor_total += description.descriptor_count;
    }

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
    resource->bindings.assign(resource_bindings.begin(), resource_bindings.end());
    // The existing GCN -> SPIR-V translator uses Shader::PushData for
    // viewport mapping, stage user data and buffer offsets. Expose the same
    // canonical 128-byte-or-smaller ABI even for descriptor-free shaders.
    // Full native Js rendering must supply *verified* values before drawing.
    const vk::PushConstantRange push_constant{
        .stageFlags = vk::ShaderStageFlagBits::eVertex |
                      vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };
    vk::DescriptorSetLayout descriptor_handle{};
    if (!layout_bindings.empty()) {
        const vk::DescriptorSetLayoutCreateInfo create_layout{
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<std::uint32_t>(layout_bindings.size()),
            .pBindings = layout_bindings.data(),
        };
        auto [desc_result, descriptors] =
            device.createDescriptorSetLayoutUnique(create_layout);
        if (desc_result != vk::Result::eSuccess)
            return false;
        resource->descriptors = std::move(descriptors);
        descriptor_handle = *resource->descriptors;
    }
    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = descriptor_handle ? 1u : 0u,
        .pSetLayouts = descriptor_handle ? &descriptor_handle : nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant,
    };
    auto [layout_result, layout] =
        device.createPipelineLayoutUnique(layout_info);
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

bool DeviceResources::QueryGraphicsPipelineLayout(
    std::uint64_t guest, std::uint64_t& handle) const {
    std::scoped_lock lock{state_->mutex};
    const auto it = state_->pipelines.find(guest);
    if (it == state_->pipelines.end() || !it->second->layout)
        return false;
    handle = Identity(static_cast<VkPipelineLayout>(*it->second->layout));
    return handle != 0;
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

bool DeviceResources::CreateSampledTexture2DMacroRgba8(
    std::uint64_t guest, std::uint32_t width, std::uint32_t height,
    std::uint32_t tiled_pitch, std::uint32_t tiled_height,
    std::uint32_t bank_swizzle, Rgba8MacroTiling mode,
    const void* source, std::uint64_t available_bytes) {
    MacroRgba8Layout layout{};
    if (!guest || !Validate2DMacroRgba8(
            width, height, tiled_pitch, tiled_height, bank_swizzle,
            available_bytes, source, mode, layout))
        return false;
    // CPU reference conversion for a narrow verified 2D GCN layout. Source
    // guest memory is never dereferenced by the GPU thread after return.
    // A later Vulkan compute detiler should replace the extra host allocation
    // for larger textures; correctness and exact addressability come first.
    try {
        std::vector<std::uint8_t> linear(
            static_cast<std::size_t>(layout.visible.total_bytes));
        Detile2DMacroRgba8(linear.data(), source, layout);
        return CreateSampledTextureRGBA8(
            guest, layout.visible.width, layout.visible.height,
            linear.data(), layout.visible.row_bytes,
            static_cast<std::uint64_t>(linear.size()));
    } catch (const std::bad_alloc&) {
        return false; // preserve the guest's existing renderer on OOM
    }
}

bool DeviceResources::CreateSampledTexture1DTiledRgba8(
    std::uint64_t guest, std::uint32_t width, std::uint32_t height,
    std::uint32_t tiled_pitch, std::uint32_t tiled_height,
    Rgba8Tiling mode, const void* source, std::uint64_t available_bytes) {
    DetileRgba8Layout layout{};
    if (!guest || !Validate1DTiledRgba8(
            width, height, tiled_pitch, tiled_height, available_bytes,
            source, mode, layout))
        return false;
    // The PS4 source pointer is NOT retained: detile immediately into owned
    // memory, then let CreateSampledTextureRGBA8 copy that into private VMA
    // staging. The source can be freed by the game immediately afterward.
    // Future streaming optimization may detile directly to VMA staging.
    try {
        std::vector<std::uint8_t> linear(
            static_cast<std::size_t>(layout.visible.total_bytes));
        Detile1DRgba8(linear.data(), source, layout);
        return CreateSampledTextureRGBA8(
            guest, layout.visible.width, layout.visible.height,
            linear.data(), layout.visible.row_bytes,
            static_cast<std::uint64_t>(linear.size()));
    } catch (const std::bad_alloc&) {
        // A failed staging allocation must leave existing native resource
        // generations and the original PS4 renderer untouched.
        return false;
    }
}

bool DeviceResources::CreateSampledTextureRGBA8(
    std::uint64_t guest, std::uint32_t width, std::uint32_t height,
    const void* source, std::uint64_t source_pitch,
    std::uint64_t source_available_bytes) {
    LinearRgba8 layout{};
    if (!guest || !ValidateLinearRgba8(width, height, source_pitch,
                                        source_available_bytes, source, layout))
        return false;
    const VkImageCreateInfo image_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo gpu_alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    auto resource = std::make_unique<State::SampledTexture>();
    if (vmaCreateImage(state_->allocator, &image_ci, &gpu_alloc_ci,
                       &resource->gpu, &resource->allocation, nullptr) != VK_SUCCESS)
        return false;
    resource->width = width;
    resource->height = height;

    const vk::ImageViewCreateInfo view_ci{
        .image = vk::Image{resource->gpu},
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto [view_result, view] =
        state_->instance.GetDevice().createImageViewUnique(view_ci);
    if (view_result != vk::Result::eSuccess || !view) {
        state_->Destroy(*resource);
        return false;
    }
    resource->view = std::move(view);

    // Copy guest-owned linear bytes NOW into a private host-visible upload
    // buffer. No original guest pointer survives into the GPU command thread.
    const VkBufferCreateInfo staging_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = layout.total_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo staging_alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VmaAllocationInfo staging_info{};
    if (vmaCreateBuffer(state_->allocator, &staging_ci, &staging_alloc_ci,
                        &resource->staging, &resource->staging_allocation,
                        &staging_info) != VK_SUCCESS) {
        state_->Destroy(*resource);
        return false;
    }
    if (!staging_info.pMappedData) {
        state_->Destroy(*resource);
        return false;
    }
    CopyRgba8Rows(staging_info.pMappedData, source, layout, source_pitch);
    if (vmaFlushAllocation(state_->allocator, resource->staging_allocation,
                           0, layout.total_bytes) != VK_SUCCESS) {
        state_->Destroy(*resource);
        return false;
    }
    if (!resource->state.Allocate()) {
        state_->Destroy(*resource);
        return false;
    }

    // Retire old image on replacement; never overwrite an in-flight VkImage.
    std::scoped_lock lock{state_->mutex};
    const auto view_handle =
        Identity(static_cast<VkImageView>(*resource->view));
    if (!state_->registry.RegisterSampledImage(guest, view_handle)) {
        state_->Destroy(*resource);
        return false;
    }
    if (auto it = state_->sampled_textures.find(guest);
        it != state_->sampled_textures.end()) {
        state_->sampled_retired.push_back(std::move(it->second));
        it->second = std::move(resource);
    } else {
        state_->sampled_textures.emplace(guest, std::move(resource));
    }
    return true;
}

bool DeviceResources::RecordSampledTextureUpload(
    std::uint64_t guest, Vulkan::Scheduler& scheduler) {
    // Called only from the GPU ownership thread, in command-submission
    // order. A texture is NOT bindable before both transfer and layout
    // transition have been enqueued on this same Vulkan scheduler.
    std::scoped_lock lock{state_->mutex};
    const auto it = state_->sampled_textures.find(guest);
    if (it == state_->sampled_textures.end() ||
        !it->second->state.BeginUpload())
        return false;
    const auto& texture = *it->second;
    const vk::Image image{texture.gpu};
    const vk::Buffer staging{texture.staging};
    const auto width = texture.width;
    const auto height = texture.height;

    scheduler.EndRendering();
    scheduler.Record([image, staging, width, height](vk::CommandBuffer cmd) {
        const vk::BufferMemoryBarrier2 host_to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eHost,
            .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging,
            .offset = 0,
            .size = std::uint64_t(width) * height * 4u,
        };
        const vk::ImageMemoryBarrier2 to_transfer{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = vk::AccessFlags2{},
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        const vk::DependencyInfo start{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &host_to_copy,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_transfer,
        };
        cmd.pipelineBarrier2(start);
        const vk::BufferImageCopy copy{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, 1},
        };
        cmd.copyBufferToImage(staging, image, vk::ImageLayout::eTransferDstOptimal,
                              copy);
        const vk::ImageMemoryBarrier2 to_shader{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics |
                            vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        const vk::DependencyInfo finish{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_shader,
        };
        cmd.pipelineBarrier2(finish);
    });
    // This is *submission-order* readiness. The GPU may still be executing
    // the upload, but a subsequent Vk draw on this scheduler executes after
    // the transfer and its visibility barrier.
    if (!it->second->state.MarkShaderReadable())
        return false;
    return state_->registry.MarkSampledImageReady(
        guest, Identity(static_cast<VkImageView>(*it->second->view)));
}

bool DeviceResources::CreateNearestClampSampler(std::uint64_t guest) {
    if (!guest) return false;
    const vk::SamplerCreateInfo ci{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    };
    auto [result, handle] =
        state_->instance.GetDevice().createSamplerUnique(ci);
    if (result != vk::Result::eSuccess || !handle)
        return false;
    const auto identity = Identity(static_cast<VkSampler>(*handle));
    auto resource = std::make_unique<State::NativeSampler>();
    resource->handle = std::move(handle);
    std::scoped_lock lock{state_->mutex};
    if (!state_->registry.RegisterSampler(guest, identity))
        return false;
    if (auto it = state_->native_samplers.find(guest);
        it != state_->native_samplers.end()) {
        state_->samplers_retired.push_back(std::move(it->second));
        it->second = std::move(resource);
    } else {
        state_->native_samplers.emplace(guest, std::move(resource));
    }
    return true;
}

bool DeviceResources::QuerySampledBinding(
    std::uint64_t texture_id, std::uint64_t sampler_id,
    SampledImageInfo& output) const {
    std::scoped_lock lock{state_->mutex};
    const auto it = state_->sampled_textures.find(texture_id);
    const auto sampler = state_->native_samplers.find(sampler_id);
    if (it == state_->sampled_textures.end() ||
        sampler == state_->native_samplers.end() ||
        !it->second->state.Ready(it->second->state.Generation()))
        return false;
    ResourceRegistry::SampledBinding binding{};
    if (!state_->registry.TryResolveSampledBinding(texture_id, sampler_id, binding))
        return false;
    if (binding.image_view !=
        Identity(static_cast<VkImageView>(*it->second->view)) ||
        binding.sampler !=
        Identity(static_cast<VkSampler>(*sampler->second->handle)))
        return false;
    output = {
        .image_view = binding.image_view,
        .sampler = binding.sampler,
        .width = it->second->width,
        .height = it->second->height,
        .image_generation = binding.image_generation,
        .sampler_generation = binding.sampler_generation,
    };
    return true;
}

// Records complete descriptor writes from native Vulkan allocations only.
// The shader layout is immutable on the live native pipeline. Each resource
// must match an independently captured guest generation and a ready Vulkan
// allocation. This does not inspect PS4 shader registers or PM4 commands.
// Call on the GPU submission owner before the draw/dispatch that consumes it.
bool DeviceResources::RecordNativeImageDescriptors(
    std::uint64_t pipeline_id, NativePipelineKind pipeline_kind,
    std::span<const NativeGuestDescriptorBinding> assignments,
    Vulkan::Scheduler& scheduler) {
    if (!pipeline_id ||
        (pipeline_kind != NativePipelineKind::Graphics &&
         pipeline_kind != NativePipelineKind::Compute))
        return false;

    struct ResolvedWrite {
        std::uint32_t binding = 0;
        vk::DescriptorType type{};
        std::vector<vk::DescriptorImageInfo> images{};
    };

    // Keep one lock order: device resources -> identity registry. The callback
    // captures handles and image infos by value; deferred recording threads
    // never dereference a temporary Js pointer, state map, or stack descriptor.
    std::scoped_lock lock{state_->mutex};
    const auto& pipelines = pipeline_kind == NativePipelineKind::Graphics
        ? state_->pipelines : state_->compute_pipelines;
    const auto it = pipelines.find(pipeline_id);
    if (it == pipelines.end() || !it->second->pipeline ||
        !it->second->layout ||
        !ValidateNativeGuestDescriptors(it->second->bindings, assignments))
        return false;
    if (assignments.empty())
        return true;
    if (!it->second->descriptors)
        return false;

    std::vector<ResolvedWrite> resolved;
    resolved.reserve(assignments.size());
    for (const auto& assignment : assignments) {
        ResolvedWrite write{};
        write.binding = assignment.binding;
        switch (assignment.kind) {
        case NativeDescriptorKind::SampledImage:
            write.type = vk::DescriptorType::eSampledImage;
            break;
        case NativeDescriptorKind::Sampler:
            write.type = vk::DescriptorType::eSampler;
            break;
        default:
            // Storage buffers, storage images, and unknown resource types
            // require their own native owner and synchronization path.
            return false;
        }
        write.images.reserve(assignment.resources.size());
        for (const auto& requested : assignment.resources) {
            ResourceRegistry::SampledIdentity identity{};
            if (assignment.kind == NativeDescriptorKind::SampledImage) {
                const auto texture = state_->sampled_textures.find(requested.guest_id);
                if (texture == state_->sampled_textures.end() ||
                    !texture->second->view ||
                    !texture->second->state.Ready(
                        texture->second->state.Generation()) ||
                    !state_->registry.TryResolveSampledImage(
                        requested.guest_id, identity) ||
                    identity.generation != requested.generation ||
                    identity.vk_handle !=
                        Identity(static_cast<VkImageView>(*texture->second->view)))
                    return false;
                write.images.push_back(vk::DescriptorImageInfo{
                    .imageView = *texture->second->view,
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                });
            } else {
                const auto sampler = state_->native_samplers.find(requested.guest_id);
                if (sampler == state_->native_samplers.end() ||
                    !sampler->second->handle ||
                    !state_->registry.TryResolveSampler(requested.guest_id, identity) ||
                    identity.generation != requested.generation ||
                    identity.vk_handle !=
                        Identity(static_cast<VkSampler>(*sampler->second->handle)))
                    return false;
                write.images.push_back(vk::DescriptorImageInfo{
                    .sampler = *sampler->second->handle,
                });
            }
        }
        resolved.push_back(std::move(write));
    }

    const vk::PipelineLayout layout = *it->second->layout;
    const vk::PipelineBindPoint bind_point =
        pipeline_kind == NativePipelineKind::Compute
            ? vk::PipelineBindPoint::eCompute
            : vk::PipelineBindPoint::eGraphics;
    scheduler.Record([layout, bind_point, resolved = std::move(resolved)]
                     (vk::CommandBuffer cmd) {
        std::vector<vk::WriteDescriptorSet> writes;
        writes.reserve(resolved.size());
        for (const auto& resource : resolved) {
            writes.push_back(vk::WriteDescriptorSet{
                .dstBinding = resource.binding,
                .descriptorCount = static_cast<std::uint32_t>(resource.images.size()),
                .descriptorType = resource.type,
                .pImageInfo = resource.images.data(),
            });
        }
        cmd.pushDescriptorSetKHR(bind_point, layout, 0, writes);
    });
    return true;
}

void DeviceResources::RetireSampledTexture(std::uint64_t guest) {
    std::scoped_lock lock{state_->mutex};
    auto it = state_->sampled_textures.find(guest);
    if (it == state_->sampled_textures.end()) return;
    state_->registry.UnregisterSampledImage(guest);
    state_->sampled_retired.push_back(std::move(it->second));
    state_->sampled_textures.erase(it);
}
void DeviceResources::RetireSampler(std::uint64_t guest) {
    std::scoped_lock lock{state_->mutex};
    auto it = state_->native_samplers.find(guest);
    if (it == state_->native_samplers.end()) return;
    state_->registry.UnregisterSampler(guest);
    state_->samplers_retired.push_back(std::move(it->second));
    state_->native_samplers.erase(it);
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
    for (auto& texture : state_->sampled_retired) state_->Destroy(*texture);
    state_->sampled_retired.clear();
    state_->samplers_retired.clear();
}
} // namespace TlgNativeVk
