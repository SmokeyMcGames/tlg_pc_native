// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone test for the experimental Gnm -> Vulkan draw hint mechanism.
// No game files, GPU, or runtime dependencies.
#include "../gpu/shim/tlg_native_draw_bridge.h"
#include <cassert>
#include <cstdint>

int main() {
    if (!TlgNativeDraw::Enabled()) return 0;
    auto& b = TlgNativeDraw::Get();
    std::uint32_t command[] = {0xc0022d00u, 300u, 2u, 0u};
    b.Publish(command);
    assert(b.TryTake(command));
    assert(!b.TryTake(command)); // consumed hints never replay
    b.Publish(command);
    command[1] = 301u;
    assert(!b.TryTake(command)); // a modified command takes the legacy path
    command[1] = 300u;
    b.Publish(command);
    assert(b.TryTake(command));
    std::uint32_t indexed[] = {0xc0032d00u, 700u, 12u, 700u, 0u};
    b.Publish(indexed, TlgNativeDraw::Bridge::Kind::DrawIndexed,
              TlgNativeDraw::Bridge::IndexedWords);
    assert(!b.TryTake(indexed, TlgNativeDraw::Bridge::Kind::DispatchDirect,
                      TlgNativeDraw::Bridge::IndexedWords)); // type mismatch
    b.Publish(indexed, TlgNativeDraw::Bridge::Kind::DrawIndexed,
              TlgNativeDraw::Bridge::IndexedWords);
    assert(b.TryTake(indexed, TlgNativeDraw::Bridge::Kind::DrawIndexed,
                     TlgNativeDraw::Bridge::IndexedWords));
    std::uint32_t dispatch[] = {0xc0031502u, 8u, 1u, 1u, 1u};
    b.Publish(dispatch, TlgNativeDraw::Bridge::Kind::DispatchDirect,
              TlgNativeDraw::Bridge::DispatchWords);
    dispatch[3] += 1;
    assert(!b.TryTake(dispatch, TlgNativeDraw::Bridge::Kind::DispatchDirect,
                      TlgNativeDraw::Bridge::DispatchWords)); // altered payload
    dispatch[3] -= 1;
    b.Publish(dispatch, TlgNativeDraw::Bridge::Kind::DispatchDirect,
              TlgNativeDraw::Bridge::DispatchWords);
    assert(b.TryTake(dispatch, TlgNativeDraw::Bridge::Kind::DispatchDirect,
                     TlgNativeDraw::Bridge::DispatchWords));
    return 0;
}
