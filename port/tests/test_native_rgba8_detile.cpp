// SPDX-License-Identifier: GPL-2.0-or-later
// Exact PS4 32bpp 1D microtile addressing, source size, and detile tests.
// No GPU, shader compiler, game files or drivers required.
#include "../gpu/shim/tlg_native_rgba8_detile.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

using namespace TlgNativeVk;

static void FillTiled(std::vector<std::uint8_t>& raw, unsigned pitch,
                      unsigned allocated_height, Rgba8Tiling mode) {
    assert((pitch & 7u) == 0 && (allocated_height & 7u) == 0);
    for (unsigned y = 0; y < allocated_height; ++y) {
        for (unsigned x = 0; x < pitch; ++x) {
            const auto tile = (y / 8u) * (pitch / 8u) + (x / 8u);
            const auto loc = 4u * (tile * 64u + PixelInMicroTile1DRgba8(x,y,mode));
            raw[loc]     = static_cast<std::uint8_t>(x);
            raw[loc + 1] = static_cast<std::uint8_t>(y);
            raw[loc + 2] = static_cast<std::uint8_t>(x ^ y);
            raw[loc + 3] = 255;
        }
    }
}

int main() {
    const auto thin = Rgba8Tiling::Thin1DThin;
    const auto display = Rgba8Tiling::Display1DThin;
    // Independent shader-derived coordinate checks. In thin tiling:
    // x0,y0,x1,y1,x2,y2; in display 32bpp: x0,x1,y0,x2,y1,y2.
    assert(PixelInMicroTile1DRgba8(0,0,thin) == 0);
    assert(PixelInMicroTile1DRgba8(1,0,thin) == 1);
    assert(PixelInMicroTile1DRgba8(0,1,thin) == 2);
    assert(PixelInMicroTile1DRgba8(2,0,thin) == 4);
    assert(PixelInMicroTile1DRgba8(0,2,thin) == 8);
    assert(PixelInMicroTile1DRgba8(4,0,thin) == 16);
    assert(PixelInMicroTile1DRgba8(0,4,thin) == 32);
    assert(PixelInMicroTile1DRgba8(7,7,thin) == 63);
    assert(PixelInMicroTile1DRgba8(0,1,display) == 4);
    assert(PixelInMicroTile1DRgba8(2,0,display) == 2);
    assert(PixelInMicroTile1DRgba8(4,0,display) == 8);
    assert(PixelInMicroTile1DRgba8(0,2,display) == 16);
    assert(PixelInMicroTile1DRgba8(0,4,display) == 32);
    assert(PixelInMicroTile1DRgba8(7,7,display) == 63);

    for (auto mode : {thin, display}) {
        for (std::array<unsigned,4> dims :
             {std::array<unsigned,4>{8,8,8,8},
              {11,9,16,16},{17,19,24,24},{31,32,32,32},
              {1,1,8,8},{8,17,8,24}}) {
            const auto [width,height,pitch,padded_height] = dims;
            std::vector<std::uint8_t> input(pitch*padded_height*4u,0);
            FillTiled(input,pitch,padded_height,mode);
            DetileRgba8Layout layout{};
            assert(Validate1DTiledRgba8(width,height,pitch,padded_height,
                                        input.size(),input.data(),mode,layout));
            assert(layout.visible.total_bytes == width*height*4u);
            std::vector<std::uint8_t> output(width*height*4u+16u,0x5a);
            Detile1DRgba8(output.data(),input.data(),layout);
            for (unsigned y=0;y<height;y++) {
                for (unsigned x=0;x<width;x++) {
                    const auto addr = 4u*(y*width+x);
                    assert(output[addr]==static_cast<std::uint8_t>(x));
                    assert(output[addr+1]==static_cast<std::uint8_t>(y));
                    assert(output[addr+2]==static_cast<std::uint8_t>(x^y));
                    assert(output[addr+3]==255);
                }
            }
            for (unsigned i=width*height*4u;i<output.size();i++)
                assert(output[i]==0x5a);
        }
    }
    std::array<std::uint8_t,1024> data{};
    DetileRgba8Layout layout{};
    assert(!Validate1DTiledRgba8(8,8,7,8,1024,data.data(),thin,layout));
    assert(!Validate1DTiledRgba8(8,8,8,7,1024,data.data(),thin,layout));
    assert(!Validate1DTiledRgba8(9,8,8,8,1024,data.data(),thin,layout));
    assert(!Validate1DTiledRgba8(8,9,8,8,1024,data.data(),thin,layout));
    assert(!Validate1DTiledRgba8(8,8,8,8,255,data.data(),thin,layout));
    assert(!Validate1DTiledRgba8(0,8,8,8,1024,data.data(),thin,layout));
    assert(!Validate1DTiledRgba8(8,0,8,8,1024,data.data(),thin,layout));
    assert(!Validate1DTiledRgba8(8193,8,8200,8,1024,data.data(),thin,layout));
    assert(!Validate1DTiledRgba8(8,8,8,8,1024,nullptr,thin,layout));
    assert(!Validate1DTiledRgba8(8,8,8,8,1024,data.data(),
                                 static_cast<Rgba8Tiling>(14),layout));
    // 8192x8192 exactly meets 256 MiB cap; anything larger is rejected.
    assert(!Validate1DTiledRgba8(8192,8192,8192,8200,~0ull,
                                 data.data(),thin,layout));
    return 0;
}
