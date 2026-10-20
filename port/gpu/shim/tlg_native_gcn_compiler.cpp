// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_gcn_compiler.h"
#include <exception>
#include <algorithm>
#include <span>
#include "shader_recompiler/info.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"

namespace TlgNativeVk {
ShaderTranslationResult TranslateVerifiedGcn(
    const GcnShaderInput& input,
    const Shader::RuntimeInfo& provided_runtime_info,
    const Shader::Profile& profile,
    Shader::Backend::Bindings initial_bindings) {
    ShaderTranslationResult result{};
    if (!input.code_verified || !input.user_data_verified) {
        result.status = ShaderTranslationStatus::UnverifiedGuestShader;
        return result;
    }
    if (!input.runtime_state_verified) {
        result.status = ShaderTranslationStatus::UnverifiedRuntimeState;
        return result;
    }
    // The native backend currently knows these three high-level Js shader
    // stages. Tessellation/geometry/export stages need verified linkage.
    const bool stage =
        (input.hw_stage == Shader::HwStage::Vertex &&
         input.sw_stage == Shader::SwStage::Vertex) ||
        (input.hw_stage == Shader::HwStage::Fragment &&
         input.sw_stage == Shader::SwStage::Fragment) ||
        (input.hw_stage == Shader::HwStage::Compute &&
         input.sw_stage == Shader::SwStage::Compute);
    if (!stage) {
        result.status = ShaderTranslationStatus::UnsupportedStage;
        return result;
    }
    if (!input.shader_hash || input.code.size() < 4 ||
        input.code.size() > (1u << 20) ||
        initial_bindings.unified > MaxNativeDescriptorSlots ||
        initial_bindings.buffer > Shader::NUM_BUFFERS ||
        initial_bindings.user_data > Shader::NUM_USER_DATA_REGS) {
        result.status = ShaderTranslationStatus::InvalidCode;
        return result;
    }
    try {
        const std::span<const std::uint32_t, 16> user_data{input.user_data};
        const Shader::ShaderParams params{
            .user_data = user_data, .code = input.code, .hash = input.shader_hash,
        };
        auto runtime = provided_runtime_info;
        Shader::Info info{input.hw_stage, input.sw_stage, params};
        Shader::Pools pools;
        auto program = Shader::TranslateProgram(input.code, pools, info,
                                                runtime, profile);
        result.buffers = static_cast<std::uint32_t>(info.buffers.size());
        result.images = static_cast<std::uint32_t>(info.images.size());
        result.samplers = static_cast<std::uint32_t>(info.samplers.size());
        result.fmasks = static_cast<std::uint32_t>(info.fmasks.size());
        // The emitter allocates set-0 slots in this exact order. The guest's
        // independently verified per-stage descriptor contract must match
        // the compiler reflection byte-for-byte before SPIR-V can be used.
        // Special emulator buffers, fmask and fetch shaders still require
        // separate native memory/vertex ownership and are not silently mapped.
        if (result.fmasks || info.has_fetch_shader ||
            std::any_of(info.buffers.begin(), info.buffers.end(),
                        [](const Shader::BufferResource& b) { return b.IsSpecial(); }) ||
            std::any_of(info.images.begin(), info.images.end(),
                        [](const Shader::ImageResource& i) {
                            return i.mip_fallback_mode != Shader::MipStorageFallbackMode::None;
                        }) ||
            result.buffers > Shader::NUM_BUFFERS - initial_bindings.buffer ||
            info.ud_mask.NumRegs() >
                Shader::NUM_USER_DATA_REGS - initial_bindings.user_data) {
            result.status = ShaderTranslationStatus::RequiresDescriptorBindings;
            return result;
        }
        for (const auto& buffer : info.buffers) {
            (void)buffer;
            if (!AppendNativeDescriptor(result.descriptor_layout,
                                        initial_bindings.unified,
                                        NativeDescriptorKind::StorageBuffer)) {
                result.status = ShaderTranslationStatus::RequiresDescriptorBindings;
                return result;
            }
        }
        for (const auto& image : info.images) {
            if (!AppendNativeDescriptor(result.descriptor_layout,
                                        initial_bindings.unified,
                                        image.is_written
                                            ? NativeDescriptorKind::StorageImage
                                            : NativeDescriptorKind::SampledImage)) {
                result.status = ShaderTranslationStatus::RequiresDescriptorBindings;
                return result;
            }
        }
        for (const auto& sampler : info.samplers) {
            (void)sampler;
            if (!AppendNativeDescriptor(result.descriptor_layout,
                                        initial_bindings.unified,
                                        NativeDescriptorKind::Sampler)) {
                result.status = ShaderTranslationStatus::RequiresDescriptorBindings;
                return result;
            }
        }
        if ((!result.descriptor_layout.empty() &&
             !input.descriptor_layout_verified) ||
            !MatchNativeDescriptorLayout(input.expected_descriptors,
                                         result.descriptor_layout,
                                         initial_bindings.unified)) {
            result.status = ShaderTranslationStatus::DescriptorLayoutMismatch;
            return result;
        }
        // The offset is passed through from the earlier shader stage so the
        // fragment stage never reuses a vertex-stage descriptor number.
        auto bindings = initial_bindings;
        result.spirv = Shader::Backend::SPIRV::EmitSPIRV(
            profile, runtime, program, bindings);
        std::uint32_t reflected_end = initial_bindings.unified;
        if (!result.descriptor_layout.empty()) {
            const auto& last = result.descriptor_layout.back();
            reflected_end = last.binding + last.descriptor_count;
        }
        if (bindings.unified != reflected_end ||
            bindings.buffer != initial_bindings.buffer + result.buffers ||
            bindings.user_data >
                Shader::NUM_USER_DATA_REGS) {
            result.status = ShaderTranslationStatus::DescriptorLayoutMismatch;
            result.spirv.clear();
            return result;
        }
        if (result.spirv.size() < 5 || result.spirv[0] != 0x07230203u) {
            result.status = ShaderTranslationStatus::InvalidSpirv;
            result.spirv.clear();
            return result;
        }
        result.bindings = bindings;
        result.status = ShaderTranslationStatus::Success;
        return result;
    } catch (const std::exception&) {
        // A failed or unsupported GCN shader must not be installed as a Vulkan
        // shader module. The native renderer leaves original rendering intact.
        result.status = ShaderTranslationStatus::TranslationFailed;
        result.spirv.clear();
        return result;
    }
}
} // namespace TlgNativeVk
