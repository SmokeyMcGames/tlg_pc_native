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
std::atomic<TlgGpuNativeJsEventPop> g_event_pop{nullptr};
std::atomic<void*> g_event_user{nullptr};
std::atomic<std::uint64_t> g_indexed{0}, g_instanced{0}, g_dropped{0};
std::atomic<std::uint64_t> g_invalid{0}, g_batches{0}, g_last_context{0}, g_last_sequence{0};
std::atomic<std::uint64_t> g_buffer_creates{0}, g_last_buffer_description{0};
}

extern "C" void tlggpu_set_native_js_draw_source(TlgGpuNativeJsPop pop, void* user) {
    // The loader registers once before entering the guest; null disables capture.
    // Publish context first, then publish the callback with release semantics.
    g_user.store(user, std::memory_order_relaxed);
    g_pop.store(pop, std::memory_order_release);
}

extern "C" void tlggpu_set_native_js_event_source(TlgGpuNativeJsEventPop pop, void* user) {
    // Register context before publishing the new source to the GPU reader.
    g_event_user.store(user, std::memory_order_relaxed);
    g_event_pop.store(pop, std::memory_order_release);
}

namespace TlgNativeJs {
void PumpOnGpuSubmission() {
    const TlgGpuNativeJsEventPop event_pop = g_event_pop.load(std::memory_order_acquire);
    const TlgGpuNativeJsPop pop = g_pop.load(std::memory_order_acquire);
    if (!event_pop && !pop) return;
    void* user = event_pop ? g_event_user.load(std::memory_order_relaxed)
                           : g_user.load(std::memory_order_relaxed);
    constexpr unsigned MaxPerSubmission = 8192;
    unsigned processed = 0;
    std::uint64_t discarded = 0;
    std::uint64_t indexed = 0, instanced = 0, invalid = 0, drops = 0;
    std::uint64_t buffer_creates = 0, last_description = 0;
    std::uint64_t last_context = 0, last_sequence = 0;
    TlgJsNativeDrawCandidate cmd{};
    TlgJsNativeCommand raw{};
    for (; processed < MaxPerSubmission; ++processed) {
        discarded = 0;
        if (event_pop) {
            if (!event_pop(user, &raw, &discarded)) {
                drops += discarded;
                break;
            }
            // The creation event preserves the original pointer arguments.
            // They are opaque and may be invalid after the guest returns;
            // never dereference any of them here.
            if (raw.kind == TLG_JS_NATIVE_CREATE_BUFFER) {
                TlgJsNativeBufferCreateCandidate created{};
                if (tlg_js_native_decode_buffer_create(&raw, &created)) {
                    ++buffer_creates;
                    last_description = created.description;
                    last_sequence = created.sequence;
                } else {
                    ++invalid;
                }
                drops += discarded;
                continue;
            }
            if (!tlg_js_native_decode_draw(&raw, &cmd)) {
                ++invalid;
                drops += discarded;
                continue;
            }
        } else if (!pop(user, &cmd, &discarded)) {
            drops += discarded;
            break;
        }
        drops += discarded;
        switch (cmd.kind) {
        case TLG_JS_NATIVE_INDEXED:
            ++indexed;
            break;
        case TLG_JS_NATIVE_INSTANCED:
            ++instanced;
            break;
        default:
            ++invalid;
            continue;
        }
        last_context = cmd.context;
        last_sequence = cmd.sequence;
    }
    // Keep the Vulkan command processor off the inter-core atomic hot path:
    // one aggregation per submission instead of one atomic op per game draw.
    if (indexed) g_indexed.fetch_add(indexed, std::memory_order_relaxed);
    if (instanced) g_instanced.fetch_add(instanced, std::memory_order_relaxed);
    if (buffer_creates) {
        g_buffer_creates.fetch_add(buffer_creates, std::memory_order_relaxed);
        g_last_buffer_description.store(last_description, std::memory_order_relaxed);
    }
    if (invalid) g_invalid.fetch_add(invalid, std::memory_order_relaxed);
    if (drops) g_dropped.fetch_add(drops, std::memory_order_relaxed);
    if (last_sequence) {
        g_last_context.store(last_context, std::memory_order_relaxed);
        g_last_sequence.store(last_sequence, std::memory_order_relaxed);
    }
    g_batches.fetch_add(1, std::memory_order_relaxed);
    static auto last_report = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(5)) {
        last_report = now;
        const auto stats = Snapshot();
        std::printf("Native Js shadow Vulkan stream: indexed=%llu instanced=%llu "
                    "CreateBuffer=%llu dropped=%llu invalid=%llu batches=%llu last=%llu; "
                    "original GPU renderer still active\n",
                    static_cast<unsigned long long>(stats.indexed),
                    static_cast<unsigned long long>(stats.indexed_instanced),
                    static_cast<unsigned long long>(stats.buffer_creates),
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
        .buffer_creates = g_buffer_creates.load(std::memory_order_relaxed),
        .last_buffer_description = g_last_buffer_description.load(std::memory_order_relaxed),
        .dropped = g_dropped.load(std::memory_order_relaxed),
        .batches = g_batches.load(std::memory_order_relaxed),
        .invalid = g_invalid.load(std::memory_order_relaxed),
        .last_context = g_last_context.load(std::memory_order_relaxed),
        .last_sequence = g_last_sequence.load(std::memory_order_relaxed)
    };
}

void ResetForTest() {
    tlggpu_set_native_js_draw_source(nullptr, nullptr);
    tlggpu_set_native_js_event_source(nullptr, nullptr);
    g_indexed.store(0); g_instanced.store(0); g_dropped.store(0);
    g_buffer_creates.store(0); g_last_buffer_description.store(0);
    g_batches.store(0); g_invalid.store(0);
    g_last_context.store(0); g_last_sequence.store(0);
}
} // namespace TlgNativeJs
