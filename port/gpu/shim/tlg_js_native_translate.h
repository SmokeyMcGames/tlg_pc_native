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
    std::uint64_t index_capacity = 0; // index elements, NOT bytes
    std::uint32_t vertex_base = 0;
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
    // Index-capacity bound also catches integer wrap of firstIndex + indexCount.
    if (source.first_index > bindings.index_capacity ||
        source.index_count > bindings.index_capacity - source.first_index)
        return false;
    destination = Draw{
        .indexed = true,
        .vertex_or_index_count = source.index_count,
        .instance_count = source.instance_count,
        .first_vertex = bindings.vertex_base,
        .first_instance = bindings.first_instance,
        .first_index = source.first_index,
    };
    return true;
}
} // namespace TlgNativeVk
