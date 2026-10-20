// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_gcn_compiler.h"
#include <exception>
#include <span>
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"

namespace TlgNativeVk {
ShaderTranslationResult TranslateVerifiedGcn(
    const GcnShaderInput& input,
    const Shader::RuntimeInfo& provided_runtime_info,
    const Shader::Profile& profile) {
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
        input.code.size() > (1u << 20)) {
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
        // The independent native pipeline currently supports descriptor-free
        // shaders only. Do NOT silently bind shaders to an empty VkLayout.
        // Later native descriptors must reflect exact game buffer/texture
        // usages, including aliasing and guest GPU/CPU memory synchronization.
        if (result.buffers || result.images || result.samplers ||
            result.fmasks || info.has_fetch_shader) {
            result.status = ShaderTranslationStatus::RequiresDescriptorBindings;
            return result;
        }
        auto bindings = Shader::Backend::Bindings{};
        result.spirv = Shader::Backend::SPIRV::EmitSPIRV(
            profile, runtime, program, bindings);
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
