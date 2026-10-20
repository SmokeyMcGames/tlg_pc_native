// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Shadow consumer for semantic JsDeviceContext draw calls.
// The original GPU interpreter remains authoritative until native resource
// binding, pipeline state and guest-memory coherence pass parity tests.
#include <cstdint>
namespace TlgNativeJs {
struct Stats {
    std::uint64_t nonindexed{};
    std::uint64_t prepare_draws{};
    std::uint64_t matched_prepare_draws{};
    std::uint64_t invalidated_preparations{};
    std::uint64_t shader_slot_4678{}, shader_slot_4680{}, shader_slot_4688{};
    std::uint64_t indexed{};
    std::uint64_t indexed_instanced{};
    std::uint64_t buffer_creates{};
    std::uint64_t pixel_shader_creates{};
    std::uint64_t srv_creates{};
    std::uint64_t last_srv_resource{};
    std::uint64_t last_srv_descriptor{};
    std::uint64_t last_shader_head{};
    std::uint64_t last_shader_tail{};
    std::uint64_t last_buffer_description{};
    std::uint32_t last_buffer_bytes{};
    std::uint32_t last_buffer_usage{};
    std::uint32_t last_buffer_bind_flags{};
    std::uint32_t last_buffer_cpu_access{};
    std::uint64_t dropped{};
    std::uint64_t batches{};
    std::uint64_t invalid{};
    std::uint64_t last_context{};
    std::uint64_t last_sequence{};
};
void PumpOnGpuSubmission();
Stats Snapshot();
void ResetForTest();
} // namespace TlgNativeJs
