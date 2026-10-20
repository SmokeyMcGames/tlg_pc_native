// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Shadow consumer for semantic JsDeviceContext draw calls.
// The original GPU interpreter remains authoritative until native resource
// binding, pipeline state and guest-memory coherence pass parity tests.
#include <cstdint>
namespace TlgNativeJs {
struct Stats {
    std::uint64_t indexed{};
    std::uint64_t indexed_instanced{};
    std::uint64_t buffer_creates{};
    std::uint64_t last_buffer_description{};
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
