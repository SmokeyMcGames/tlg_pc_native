// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Native Vulkan resource ownership and binding validation for JsDeviceContext.
//
// The registry never reads PM4 registers or dereferences game pointers.
// A guest object is just an opaque integer key. Vulkan objects are identified
// by driver-created handles supplied through future JsDevice resource hooks.
// They MUST NOT be treated as valid until created by the native Vulkan backend.
//
// This layer prevents alias/reuse of a guest pointer from accidentally binding
// a stale GPU object. A Draw is made only with a coherent pipeline/index/target
// snapshot and an explicit synchronization acknowledgment from the GPU owner.
#include "tlg_js_native_translate.h"
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace TlgNativeVk {

struct NativeResource {
    std::uint64_t vk_handle = 0;
    std::uint64_t generation = 0;
    std::uint64_t capacity = 0;  // index elements for buffers
    std::uint64_t write_epoch = 0;
};

struct NativeContextState {
    std::uint64_t pipeline = 0; // opaque guest-owned graphics pipeline
    std::uint64_t indices = 0;  // opaque guest-owned index buffer
    std::uint64_t targets = 0;  // opaque guest-owned render target set
    std::uint64_t pipeline_generation = 0;
    std::uint64_t index_generation = 0;
    std::uint64_t target_generation = 0;
    std::uint64_t barrier_epoch = 0;
    std::uint32_t index_binding_base = 0;
    std::uint32_t vertex_base = 0;
    std::uint32_t first_instance = 0;
    bool barriers_recorded = false;
};

struct NativeBoundDraw {
    Draw command{};
    std::uint64_t pipeline = 0;
    std::uint64_t index_buffer = 0;
    std::uint64_t target_set = 0;
    std::uint32_t index_binding_base = 0;
};

class ResourceRegistry {
public:
    // Backend-created Vulkan handles ONLY. Update invalidates all bound
    // snapshots referencing an older generation of this guest object.
    bool RegisterPipeline(std::uint64_t guest, std::uint64_t vk_handle) {
        return Register(pipelines_, guest, vk_handle, 0);
    }
    bool RegisterIndexBuffer(std::uint64_t guest, std::uint64_t vk_handle,
                             std::uint64_t indices) {
        return indices && Register(buffers_, guest, vk_handle, indices);
    }
    bool RegisterTargetSet(std::uint64_t guest, std::uint64_t vk_handle) {
        return Register(targets_, guest, vk_handle, 0);
    }
    void UnregisterPipeline(std::uint64_t guest) { Remove(pipelines_, guest); }
    void UnregisterIndexBuffer(std::uint64_t guest) { Remove(buffers_, guest); }
    void UnregisterTargetSet(std::uint64_t guest) { Remove(targets_, guest); }

    bool Bind(std::uint64_t context, std::uint64_t pipeline,
              std::uint64_t indices, std::uint64_t targets,
              std::uint32_t index_binding_base, std::uint32_t vertex_base,
              std::uint32_t first_instance = 0) {
        if (!context || !pipeline || !indices || !targets) return false;
        std::scoped_lock lock{mutex_};
        const auto p = pipelines_.find(pipeline);
        const auto i = buffers_.find(indices);
        const auto t = targets_.find(targets);
        if (p == pipelines_.end() || i == buffers_.end() ||
            t == targets_.end() ||
            index_binding_base > i->second.capacity)
            return false;
        contexts_[context] = {
            .pipeline = pipeline, .indices = indices, .targets = targets,
            .pipeline_generation = p->second.generation,
            .index_generation = i->second.generation,
            .target_generation = t->second.generation,
            .barrier_epoch = 0,
            .index_binding_base = index_binding_base,
            .vertex_base = vertex_base,
            .first_instance = first_instance,
            .barriers_recorded = false,
        };
        return true;
    }

    // A native buffer upload invalidates previous synchronization and must be
    // observed before the buffer can be read by Vulkan. Caller records real
    // VkBufferMemoryBarrier2 and signals completed GPU epoch; this only tracks it.
    void NoteBufferWrite(std::uint64_t guest, std::uint64_t new_write_epoch) {
        std::scoped_lock lock{mutex_};
        auto it = buffers_.find(guest);
        if (it != buffers_.end())
            it->second.write_epoch = std::max(it->second.write_epoch,
                                              new_write_epoch);
    }
    bool AcknowledgeBarriers(std::uint64_t context, std::uint64_t completed_epoch) {
        std::scoped_lock lock{mutex_};
        auto it = contexts_.find(context);
        if (it == contexts_.end()) return false;
        it->second.barrier_epoch = completed_epoch;
        it->second.barriers_recorded = true;
        return true;
    }
    void Unbind(std::uint64_t context) {
        std::scoped_lock lock{mutex_};
        contexts_.erase(context);
    }
    // Resolve a semantic game draw without a single read from the PS4 PM4
    // register file. A failed lookup MUST fall back to the existing renderer.
    bool TryResolve(const TlgJsNativeDrawCandidate& game,
                    NativeBoundDraw& output) const {
        std::scoped_lock lock{mutex_};
        const auto ctx = contexts_.find(game.context);
        if (ctx == contexts_.end() || !ctx->second.barriers_recorded)
            return false;
        const auto& c = ctx->second;
        const auto p = pipelines_.find(c.pipeline);
        const auto i = buffers_.find(c.indices);
        const auto t = targets_.find(c.targets);
        if (p == pipelines_.end() || i == buffers_.end() ||
            t == targets_.end() ||
            p->second.generation != c.pipeline_generation ||
            i->second.generation != c.index_generation ||
            t->second.generation != c.target_generation ||
            i->second.write_epoch > c.barrier_epoch)
            return false;
        const std::uint64_t visible_capacity = i->second.capacity - c.index_binding_base;
        const ResolvedJsGraphics ready{
            .guest_context = game.context,
            .shader_generation = p->second.generation,
            .resource_generation = i->second.generation,
            .index_capacity = visible_capacity,
            .index_binding_base = c.index_binding_base,
            .vertex_base = c.vertex_base,
            .first_instance = c.first_instance,
            .pipeline_bound = p->second.vk_handle != 0,
            .index_buffer_bound = i->second.vk_handle != 0,
            .render_targets_ready = t->second.vk_handle != 0,
            .barriers_ready = true,
        };
        Draw draw{};
        if (!MakeJsDraw(game, ready, draw)) return false;
        output = {
            .command = draw,
            .pipeline = p->second.vk_handle,
            .index_buffer = i->second.vk_handle,
            .target_set = t->second.vk_handle,
            .index_binding_base = c.index_binding_base,
        };
        return true;
    }

private:
    using Table = std::unordered_map<std::uint64_t, NativeResource>;
    bool Register(Table& table, std::uint64_t guest, std::uint64_t handle,
                  std::uint64_t capacity) {
        if (!guest || !handle) return false;
        std::scoped_lock lock{mutex_};
        table[guest] = {handle, ++generation_, capacity, 0};
        return true;
    }
    void Remove(Table& table, std::uint64_t guest) {
        std::scoped_lock lock{mutex_};
        table.erase(guest);
    }
    mutable std::mutex mutex_;
    std::uint64_t generation_ = 0;
    Table pipelines_, buffers_, targets_;
    std::unordered_map<std::uint64_t, NativeContextState> contexts_;
};
} // namespace TlgNativeVk
