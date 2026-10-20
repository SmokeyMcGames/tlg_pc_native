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
    return 0;
}
