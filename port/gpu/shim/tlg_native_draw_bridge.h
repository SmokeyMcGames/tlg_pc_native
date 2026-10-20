// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Experimental high-level Gnm -> Vulkan draw bridge.
//
// sceGnmDrawIndexAuto publishes a *hint* when it builds a command. The GPU command
// thread validates the original packet, then executes the equivalent typed draw
// directly against Rasterizer (which records Vulkan commands). The original PM4
// remains intact; any miss or changed packet takes the existing decoder path.
//
// This is a first, deliberately narrow bridge, NOT a replacement for JsDevice,
// resource-state tracking, or the shader recompiler. Off unless
// TLG_NATIVE_DRAW_BRIDGE=1. It MUST NOT change game-visible memory or event order.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace TlgNativeDraw {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TLG_NATIVE_DRAW_BRIDGE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

class Bridge {
public:
    enum class Kind : std::uint8_t { DrawAuto = 1, DrawIndexed = 2, DispatchDirect = 3 };
    static constexpr unsigned PacketWords = 3; // DrawIndexAuto including PM4 header
    static constexpr unsigned IndexedWords = 5; // DrawIndexOffset2 including header
    static constexpr unsigned DispatchWords = 5; // DispatchDirect including header
    static constexpr unsigned MaxPacketWords = 5;

    // The Gnm helper writes a 3-word DrawIndexAuto packet plus a 4-word NOP.
    // Keep the complete original 7-word command unchanged. No guest pointers
    // survive beyond comparison and no Vulkan objects are cached here.
    void Publish(const std::uint32_t* packet, Kind kind = Kind::DrawAuto,
                 unsigned words = PacketWords) {
        if (!Enabled() || !packet || words == 0 || words > MaxPacketWords) {
            return;
        }
        const auto index = Slot(packet);
        std::scoped_lock lock{locks_[index % locks_.size()]};
        auto& entry = entries_[index];
        if (entry.address && entry.address != packet) {
            overwritten_.fetch_add(1, std::memory_order_relaxed);
        }
        entry.address = packet;
        entry.kind = kind;
        entry.count = words;
        entry.words = {};
        std::memcpy(entry.words.data(), packet, words * sizeof(std::uint32_t));
        published_.fetch_add(1, std::memory_order_relaxed);
    }

    // Called only after the GPU parser established a DrawIndexAuto packet with
    // exactly two payload words. A modified/reused command never trusts the
    // hint; instead the unmodified PM4 decoder runs.
    bool TryTake(const std::uint32_t* packet, Kind kind = Kind::DrawAuto,
                 unsigned words = PacketWords) {
        if (!Enabled() || !packet || words == 0 || words > MaxPacketWords) {
            return false;
        }
        const auto index = Slot(packet);
        std::scoped_lock lock{locks_[index % locks_.size()]};
        auto& entry = entries_[index];
        if (entry.address != packet) {
            missed_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::array<std::uint32_t, MaxPacketWords> current{};
        std::memcpy(current.data(), packet, words * sizeof(std::uint32_t));
        entry.address = nullptr; // never replay stale metadata on command-buffer reuse
        if (entry.kind != kind || entry.count != words || entry.words != current) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        accepted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void ReportIfDue() {
        if (!Enabled()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report_ < std::chrono::seconds(5)) {
            return;
        }
        last_report_ = now; // called only by the GPU command processor
        std::printf("Native Gnm->Vulkan draw bridge: accepted %llu, missing %llu, "
                    "modified %llu, overwritten %llu, published %llu (total)\n",
                    static_cast<unsigned long long>(accepted_.load()),
                    static_cast<unsigned long long>(missed_.load()),
                    static_cast<unsigned long long>(rejected_.load()),
                    static_cast<unsigned long long>(overwritten_.load()),
                    static_cast<unsigned long long>(published_.load()));
    }

private:
    struct Entry {
        const std::uint32_t* address = nullptr;
        Kind kind = Kind::DrawAuto;
        unsigned count = 0;
        std::array<std::uint32_t, MaxPacketWords> words{};
    };
    static constexpr std::size_t Capacity = 1u << 14;
    static constexpr std::size_t Stripes = 64;
    static std::size_t Slot(const std::uint32_t* packet) {
        const auto ptr = reinterpret_cast<std::uintptr_t>(packet) >> 2;
        return (ptr ^ (ptr >> 14)) & (Capacity - 1);
    }
    std::array<Entry, Capacity> entries_{};
    std::array<std::mutex, Stripes> locks_{};
    std::atomic<std::uint64_t> published_{0}, accepted_{0}, missed_{0}, rejected_{0}, overwritten_{0};
    std::chrono::steady_clock::time_point last_report_{std::chrono::steady_clock::now()};
};

inline Bridge& Get() {
    static Bridge bridge;
    return bridge;
}

} // namespace TlgNativeDraw
