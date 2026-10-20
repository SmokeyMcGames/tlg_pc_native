// SPDX-License-Identifier: GPL-2.0-or-later
// Exhaustive PS4 32bpp, 8-pipe/16-bank macro tile address and detile tests.
#include "../gpu/shim/tlg_native_rgba8_macro_detile.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

using namespace TlgNativeVk;

static void TestLayout(std::uint32_t width, std::uint32_t height,
                       std::uint32_t pitch, std::uint32_t padded_height,
                       Rgba8MacroTiling mode, std::uint32_t swizzle) {
    std::vector<std::uint8_t> input(std::uint64_t(pitch) * padded_height * 4u,0);
    MacroRgba8Layout layout{};
    assert(Validate2DMacroRgba8(width,height,pitch,padded_height,swizzle,
                                input.size(),input.data(),mode,layout));
    // A valid macro tile must map every padded physical pixel to a different
    // byte address. This detects bank/pipe collisions and addressing beyond
    // the caller-declared source allocation, without a reference GPU.
    std::vector<std::uint8_t> visited(input.size() / 4u,0);
    for (std::uint32_t y=0; y<padded_height; ++y) {
        for (std::uint32_t x=0; x<pitch; ++x) {
            const auto off=MacroPixelByteOffsetRgba8(x,y,layout);
            assert(off % 4u == 0 && off + 4u <= input.size());
            const auto word=off / 4u;
            assert(visited[word]==0);
            visited[word]=1;
            input[off] = static_cast<std::uint8_t>(x);
            input[off+1] = static_cast<std::uint8_t>(y);
            input[off+2] = static_cast<std::uint8_t>((x*7u+y*13u)&255u);
            input[off+3] = 255;
        }
    }
    assert(std::all_of(visited.begin(),visited.end(),
                       [](std::uint8_t value){return value==1;}));
    std::vector<std::uint8_t> output(std::uint64_t(width)*height*4u+32u,0xa5);
    Detile2DMacroRgba8(output.data(),input.data(),layout);
    for (std::uint32_t y=0;y<height;y++) {
        for (std::uint32_t x=0;x<width;x++) {
            const auto off=(std::uint64_t(y)*width+x)*4u;
            assert(output[off]==static_cast<std::uint8_t>(x));
            assert(output[off+1]==static_cast<std::uint8_t>(y));
            assert(output[off+2]==static_cast<std::uint8_t>((x*7u+y*13u)&255u));
            assert(output[off+3]==255);
        }
    }
    for(std::size_t i=std::uint64_t(width)*height*4u;i<output.size();++i)
        assert(output[i]==0xa5);
}

int main() {
    MacroRgba8Layout layout{};
    std::array<std::uint8_t,128u*64u*4u> input{};
    const auto thin=Rgba8MacroTiling::Thin2DThin;
    const auto display=Rgba8MacroTiling::Display2DThin;
    assert(Validate2DMacroRgba8(128,64,128,64,0,
                                input.size(),input.data(),thin,layout));
    assert(MacroPixelByteOffsetRgba8(0,0,layout)==0);
    assert(MacroPixelByteOffsetRgba8(8,0,layout)==256);
    assert(MacroPixelByteOffsetRgba8(0,8,layout)==16640);
    assert(MacroPixelByteOffsetRgba8(64,0,layout)==2048);
    assert(MacroPixelByteOffsetRgba8(0,16,layout)==8704);
    assert(MacroPixelByteOffsetRgba8(127,63,layout)+4<=input.size());

    for (auto mode:{thin,display}) {
        for (std::uint32_t swizzle:{0u,1u,7u,15u}) {
            TestLayout(128,64,128,64,mode,swizzle);
            TestLayout(173,97,256,128,mode,swizzle);
            TestLayout(1,1,128,64,mode,swizzle);
        }
    }
    assert(!Validate2DMacroRgba8(128,64,127,64,0,input.size(),
                                 input.data(),thin,layout));
    assert(!Validate2DMacroRgba8(128,64,128,63,0,input.size(),
                                 input.data(),thin,layout));
    assert(!Validate2DMacroRgba8(129,64,128,64,0,input.size(),
                                 input.data(),thin,layout));
    assert(!Validate2DMacroRgba8(128,64,128,64,16,input.size(),
                                 input.data(),thin,layout));
    assert(!Validate2DMacroRgba8(128,64,128,64,0,input.size()-1,
                                 input.data(),thin,layout));
    assert(!Validate2DMacroRgba8(128,64,128,64,0,input.size(),
                                 nullptr,thin,layout));
    assert(!Validate2DMacroRgba8(128,64,128,64,0,input.size(),
                                 input.data(),Rgba8MacroTiling{15},layout));
    assert(!Validate2DMacroRgba8(8192,8192,8192,8256,0,~0ull,
                                 input.data(),thin,layout));
    return 0;
}
