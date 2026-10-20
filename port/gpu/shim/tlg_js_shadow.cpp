// SPDX-License-Identifier: GPL-2.0-or-later
// Game-level renderer transition: typed high-level command shadow consumer.
//
// The Vulkan GPU-command thread drains the guest-produced Js command ring at
// every submission. Resource/pipeline binding still comes from the old renderer;
// no Vk draw may execute here until its game-level resource-state model is ready.
// The consumer does NOT read guest pointers or alter PS4 fence/event semantics.
#include "tlg_js_shadow.h"
#include "../tlggpu.h"
#include <atomic>
#include <chrono>
#include <cstdio>

namespace {
std::atomic<TlgGpuNativeJsPop> g_pop{nullptr};
std::atomic<void*> g_user{nullptr};
std::atomic<std::uint64_t> g_indexed{0}, g_instanced{0}, g_dropped{0};
std::atomic<std::uint64_t> g_invalid{0}, g_batches{0}, g_last_context{0}, g_last_sequence{0};
}

extern "C" void tlggpu_set_native_js_draw_source(TlgGpuNativeJsPop pop, void* user) {
    // The loader registers once before entering the guest; null disables capture.
    // Publish context first, then publish the callback with release semantics.
    g_user.store(user, std::memory_order_relaxed);
    g_pop.store(pop, std::memory_order_release);
}

namespace TlgNativeJs {
void PumpOnGpuSubmission() {
    const TlgGpuNativeJsPop pop = g_pop.load(std::memory_order_acquire);
    if (!pop) return;
    void* user = g_user.load(std::memory_order_relaxed);
    constexpr unsigned MaxPerSubmission = 8192;
    unsigned processed = 0;
    std::uint64_t discarded = 0;
    TlgJsNativeDrawCandidate cmd{};
    for (; processed < MaxPerSubmission; ++processed) {
        discarded = 0;
        if (!pop(user, &cmd, &discarded)) {
            if (discarded) g_dropped.fetch_add(discarded, std::memory_order_relaxed);
            break;
        }
        if (discarded) g_dropped.fetch_add(discarded, std::memory_order_relaxed);
        switch (cmd.kind) {
        case TLG_JS_NATIVE_INDEXED:
            g_indexed.fetch_add(1, std::memory_order_relaxed);
            break;
        case TLG_JS_NATIVE_INSTANCED:
            g_instanced.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            g_invalid.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        g_last_context.store(cmd.context, std::memory_order_relaxed);
        g_last_sequence.store(cmd.sequence, std::memory_order_relaxed);
    }
    g_batches.fetch_add(1, std::memory_order_relaxed);
    static auto last_report = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(5)) {
        last_report = now;
        const auto stats = Snapshot();
        std::printf("Native Js shadow Vulkan stream: indexed=%llu instanced=%llu "
                    "dropped=%llu invalid=%llu batches=%llu last=%llu; "
                    "original GPU renderer still active\n",
                    static_cast<unsigned long long>(stats.indexed),
                    static_cast<unsigned long long>(stats.indexed_instanced),
                    static_cast<unsigned long long>(stats.dropped),
                    static_cast<unsigned long long>(stats.invalid),
                    static_cast<unsigned long long>(stats.batches),
                    static_cast<unsigned long long>(stats.last_sequence));
    }
}

Stats Snapshot() {
    return {
        .indexed = g_indexed.load(std::memory_order_relaxed),
        .indexed_instanced = g_instanced.load(std::memory_order_relaxed),
        .dropped = g_dropped.load(std::memory_order_relaxed),
        .batches = g_batches.load(std::memory_order_relaxed),
        .invalid = g_invalid.load(std::memory_order_relaxed),
        .last_context = g_last_context.load(std::memory_order_relaxed),
        .last_sequence = g_last_sequence.load(std::memory_order_relaxed)
    };
}

void ResetForTest() {
    tlggpu_set_native_js_draw_source(nullptr, nullptr);
    g_indexed.store(0); g_instanced.store(0); g_dropped.store(0);
    g_batches.store(0); g_invalid.store(0);
    g_last_context.store(0); g_last_sequence.store(0);
}
} // namespace TlgNativeJs
