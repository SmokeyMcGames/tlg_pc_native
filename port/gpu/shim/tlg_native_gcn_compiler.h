// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Independent GCN -> SPIR-V translation entry for verified game-level shaders.
//
// No PM4 parsing occurs here. The existing GCN shader recompiler is reused,
// because native Vulkan does not execute PS4 GCN instructions directly.
// Only a trusted JsDevice shader source, fully verified runtime specialization
// and complete guest user-data bindings may be supplied. A raw .sb filename
// and a guessed resource layout are NOT sufficient to call Translate().
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include "tlg_native_descriptor_layout.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/backend/bindings.h"

namespace TlgNativeVk {
enum class ShaderTranslationStatus {
    Success,
    UnverifiedGuestShader,
    UnverifiedRuntimeState,
    UnsupportedStage,
    InvalidCode,
    RequiresDescriptorBindings,
    DescriptorLayoutMismatch,
    InvalidSpirv,
    TranslationFailed,
};
struct GcnShaderInput {
    Shader::HwStage hw_stage{};
    Shader::SwStage sw_stage{};
    std::span<const std::uint32_t> code{};
    std::array<std::uint32_t, 16> user_data{};
    std::uint64_t shader_hash = 0;
    bool code_verified = false;
    bool user_data_verified = false;
    bool runtime_state_verified = false;
    // Set true only after resolving immutable Js resource identities and
    // descriptor types for this exact shader specialization. Stage-specific
    // bindings use absolute SPIR-V set-0 numbers, not guessed PS4 slots.
    bool descriptor_layout_verified = false;
    std::span<const NativeDescriptorBinding> expected_descriptors{};
};
struct ShaderTranslationResult {
    ShaderTranslationStatus status{ShaderTranslationStatus::InvalidCode};
    std::vector<std::uint32_t> spirv{};
    Shader::Backend::Bindings bindings{};
    std::vector<NativeDescriptorBinding> descriptor_layout{};
    std::uint32_t buffers = 0, images = 0, samplers = 0, fmasks = 0;
};

// Reuses shadPS4's battle-tested IR and SPIR-V emitter, not its PM4 decoder.
// Resource-referencing shaders must wait until the native JsDevice resource
// registry supplies real descriptor and buffer mappings. No Vulkan objects
// are created by this translation function.
ShaderTranslationResult TranslateVerifiedGcn(
    const GcnShaderInput& input,
    const Shader::RuntimeInfo& verified_runtime_info,
    const Shader::Profile& current_vulkan_device_profile,
    Shader::Backend::Bindings initial_bindings = {});

} // namespace TlgNativeVk
