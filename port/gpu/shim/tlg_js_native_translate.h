// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Resolve a captured game-level Js draw to a resource-validated Vulkan draw.
//
// The old game's PS4 PM4 stream is not consulted here. Rendering is permitted
// ONLY after the independent native resource manager supplies a matching,
// complete binding snapshot (pipeline, index buffer, render targets, barriers).
// No guest pointers are dereferenced, and unknown state ALWAYS declines.
#include "tlg_native_vk_draw.h"
#include "../../src/native_js_commands.h"
#include <cstdint>

namespace TlgNativeVk {
struct ResolvedJsGraphics {
    std::uint64_t guest_context = 0;
    std::uint64_t shader_generation = 0;
    std::uint64_t resource_generation = 0;
    std::uint64_t index_capacity = 0; // elements from bound Vk index-buffer offset
    std::uint32_t index_binding_base = 0; // original guest index offset at the Vk binding
    std::uint32_t vertex_base = 0; // verified game's vertex-base state
    std::uint32_t first_instance = 0;
    bool pipeline_bound = false;
    bool index_buffer_bound = false;
    bool render_targets_ready = false;
    bool barriers_ready = false;
};

// Used only after Vulkan handle binding has succeeded. No real game draws
// may use this until the game's Js resource maps have been reconstructed.
inline bool MakeJsDraw(const TlgJsNativeDrawCandidate& source,
                       const ResolvedJsGraphics& bindings,
                       Draw& destination) {
    if (!source.context || source.context != bindings.guest_context ||
        !bindings.shader_generation || !bindings.resource_generation ||
        !bindings.pipeline_bound || !bindings.index_buffer_bound ||
        !bindings.render_targets_ready || !bindings.barriers_ready ||
        (source.kind != TLG_JS_NATIVE_INDEXED &&
         source.kind != TLG_JS_NATIVE_INSTANCED))
        return false;
    // Fail closed on invalid or empty batches rather than touching Vulkan state.
    if (!source.index_count || !source.instance_count)
        return false;
    // Source's vertex-base was independently reconstructed from the actual
    // guest draw ABI. Reject stale/incompatible binding state instead of
    // silently substituting the PM4 renderer's vertex offset.
    if (bindings.vertex_base != source.vertex_base)
        return false;
    // Guest index offset is relative to its index data. The Vulkan binding
    // may already have advanced into that same buffer; firstIndex is then
    // relative to the Vk-bound offset, not the guest's absolute index offset.
    if (source.first_index < bindings.index_binding_base)
        return false;
    const std::uint64_t relative =
        std::uint64_t(source.first_index - bindings.index_binding_base);
    if (relative > bindings.index_capacity ||
        source.index_count > bindings.index_capacity - relative)
        return false;
    destination = Draw{
        .indexed = true,
        .vertex_or_index_count = source.index_count,
        .instance_count = source.instance_count,
        .first_vertex = bindings.vertex_base,
        .first_instance = bindings.first_instance,
        .first_index = static_cast<std::uint32_t>(relative),
    };
    return true;
}
} // namespace TlgNativeVk
