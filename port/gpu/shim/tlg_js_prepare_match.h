// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Correlate high-level game draw entries with the subsequent JsDeviceContext
// PrepareDraw state materialization BEFORE PS4 PM4 packet encoding.
//
// All values are copied scalars from the guest entry ABI. Opaque shader slots
// are NOT Vulkan handles and this class never dereferences or submits them.
// Missing/interleaved/lost events invalidate the candidate, not the game.
//
// The GPU command processor is the sole reader, so no mutex is necessary.
#include "../../src/native_js_commands.h"
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace TlgNativeJs {
struct PreparedDrawSnapshot {
    std::uint64_t context{};
    std::uint64_t draw_sequence{}, preparation_sequence{};
    std::uint64_t slot_4678{}, slot_4680{}, slot_4688{};
    std::uint64_t prepare_arg_rsi{}, prepare_arg_rdx{};
    TlgJsNativeDrawCandidate indexed{};
    TlgJsNativeNonIndexedCandidate ordinary{};
    bool is_indexed{};
};

class PrepareDrawMatcher {
public:
    static constexpr std::size_t MaxLiveContexts = 64;

    // Returns a shadow-only draw/PrepareDraw pair. Do not record a Vulkan
    // draw merely because this returns true: PrepareDraw may still fail,
    // and no guest resource layout or Vulkan descriptor is validated.
    bool Observe(const TlgJsNativeCommand& event, PreparedDrawSnapshot& out) {
        if (event.kind == TLG_JS_NATIVE_DRAW ||
            event.kind == TLG_JS_NATIVE_INDEXED ||
            event.kind == TLG_JS_NATIVE_INSTANCED) {
            if (!event.context || event.ready == 0) return false;
            Pending p{};
            p.sequence = event.ready;
            p.is_indexed = event.kind != TLG_JS_NATIVE_DRAW;
            if (p.is_indexed) {
                if (!tlg_js_native_decode_draw(&event, &p.indexed)) return false;
            } else {
                if (!tlg_js_native_decode_nonindexed(&event, &p.ordinary)) return false;
            }
            if (pending_.size() >= MaxLiveContexts &&
                pending_.find(event.context) == pending_.end()) {
                pending_.clear(); // refuse stale cross-frame data
                ++reset_count_;
            }
            pending_[event.context] = p;
            return false;
        }
        if (event.kind != TLG_JS_NATIVE_PREPARE_DRAW) return false;
        TlgJsNativePrepareDrawCandidate state{};
        if (!tlg_js_native_decode_prepare_draw(&event, &state)) return false;
        const auto it = pending_.find(state.context);
        if (it == pending_.end()) return false;
        const Pending p = it->second;
        pending_.erase(it);
        if (p.sequence >= state.sequence) return false;
        out = {
            .context = state.context,
            .draw_sequence = p.sequence,
            .preparation_sequence = state.sequence,
            .slot_4678 = state.shader_slot_4678,
            .slot_4680 = state.shader_slot_4680,
            .slot_4688 = state.shader_slot_4688,
            .prepare_arg_rsi = state.arg_rsi,
            .prepare_arg_rdx = state.arg_rdx,
            .indexed = p.indexed,
            .ordinary = p.ordinary,
            .is_indexed = p.is_indexed,
        };
        ++paired_count_;
        return true;
    }

    void ResetAfterLoss() {
        pending_.clear();
        ++reset_count_;
    }
    std::uint64_t PairedCount() const { return paired_count_; }
    std::uint64_t ResetCount() const { return reset_count_; }
    std::size_t PendingCount() const { return pending_.size(); }
private:
    struct Pending {
        std::uint64_t sequence{};
        TlgJsNativeDrawCandidate indexed{};
        TlgJsNativeNonIndexedCandidate ordinary{};
        bool is_indexed{};
    };
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::uint64_t paired_count_ = 0, reset_count_ = 0;
};
} // namespace TlgNativeJs
