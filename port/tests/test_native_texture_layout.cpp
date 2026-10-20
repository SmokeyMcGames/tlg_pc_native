// SPDX-License-Identifier: GPL-2.0-or-later
// Linear RGBA8 staging and Vulkan texture lifecycle regression, no driver.
#include "../gpu/shim/tlg_native_texture_layout.h"
#include <array>
#include <cassert>
#include <cstdint>

int main() {
    using namespace TlgNativeVk;
    const std::array<std::uint8_t, 32> pitched{{
        1,2,3,4,5,6,7,8, 99,99,99,99,99,99,99,99,
        9,10,11,12,13,14,15,16, 77,77,77,77,77,77,77,77
    }};
    LinearRgba8 layout{};
    assert(ValidateLinearRgba8(2,2,16,pitched.size(),pitched.data(),layout));
    assert(layout.width==2 && layout.height==2);
    assert(layout.row_bytes==8 && layout.total_bytes==16);
    std::array<std::uint8_t,16> tight{};
    CopyRgba8Rows(tight.data(),pitched.data(),layout,16);
    for(unsigned i=0;i<8;i++) {
        assert(tight[i]==pitched[i]);
        assert(tight[i+8]==pitched[i+16]);
    }
    assert(!ValidateLinearRgba8(2,2,7,32,pitched.data(),layout));
    assert(!ValidateLinearRgba8(2,2,16,23,pitched.data(),layout));
    assert(!ValidateLinearRgba8(0,2,16,32,pitched.data(),layout));
    assert(!ValidateLinearRgba8(2,0,16,32,pitched.data(),layout));
    assert(!ValidateLinearRgba8(8193,2,32800,70000,pitched.data(),layout));
    assert(!ValidateLinearRgba8(2,2,16,32,nullptr,layout));
    assert(!ValidateLinearRgba8(8000,8000,32000,
                                0xffffffffffffffffull,pitched.data(),layout));
    // Declared data size is NOT trusted as a pointer permission: source gets
    // dereferenced only after all size checks and only for the valid 2x2 input.
    assert(!ValidateLinearRgba8(3,2,UINT64_MAX,UINT64_MAX,pitched.data(),layout));

    NativeTextureTransition tex;
    assert(tex.Phase()==NativeTexturePhase::Invalid);
    assert(!tex.MarkShaderReadable());
    assert(tex.Allocate());
    const auto gen=tex.Generation();
    assert(!tex.Allocate());
    assert(!tex.Ready(gen));
    assert(tex.BeginUpload());
    assert(!tex.BeginUpload());
    assert(!tex.Ready(gen));
    assert(tex.MarkShaderReadable());
    assert(tex.Ready(gen));
    assert(!tex.Ready(gen+1));
    tex.Retire();
    assert(!tex.Ready(gen));
    assert(tex.Allocate());
    assert(tex.Generation()==gen+1);
    assert(!tex.Ready(gen));
    return 0;
}
