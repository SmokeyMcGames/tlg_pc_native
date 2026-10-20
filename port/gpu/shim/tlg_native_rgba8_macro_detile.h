// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Narrow, reference-derived PS4/GCN 2D macro-tiled RGBA8 detiler.
//
// Reference: video_core/host_shaders/tiling.comp, with:
// ARRAY_2D_TILED_THIN1, MICRO_TILE_THICKNESS=1, NUM_SAMPLES=1,
// BITS_PER_PIXEL=32, PIPE_CONFIG=P8_32x32_16x16, NUM_BANKS=16,
// BANK_WIDTH=BANK_HEIGHT=1, MACRO_TILE_ASPECT=2, TILE_SPLIT_BYTES=512,
// alt_tile=false. This produces 128x64 macrotiles of 32 KiB each.
//
// The caller MUST have independently validated all tile-mode/array/pipe
// properties from the game's image descriptor. No other GCN format or mode
// is accepted. Does not implement DCC, CMASK, FMASK, BC compression,
// bank rotations, slices, mips, multisampling or sparse/PRT tiling.
#include "tlg_native_rgba8_detile.h"
#include <cstdint>
#include <cstring>

namespace TlgNativeVk {
enum class Rgba8MacroTiling : std::uint32_t {
    Display2DThin = 10, // AmdGpu::TileMode::Display2DThin
    Thin2DThin = 14,    // AmdGpu::TileMode::Thin2DThin
};
static constexpr std::uint32_t MacroWidthRgba8 = 128;
static constexpr std::uint32_t MacroHeightRgba8 = 64;
static constexpr std::uint64_t MacroBytesRgba8 = 128ull * 64 * 4;

struct MacroRgba8Layout {
    LinearRgba8 visible{};
    std::uint32_t tiled_pitch = 0;
    std::uint32_t tiled_height = 0;
    std::uint32_t bank_swizzle = 0;
    std::uint64_t required_input_bytes = 0;
    Rgba8MacroTiling mode = Rgba8MacroTiling::Thin2DThin;
};

inline bool Validate2DMacroRgba8(
    std::uint32_t width, std::uint32_t height,
    std::uint32_t tiled_pitch, std::uint32_t tiled_height,
    std::uint32_t bank_swizzle, std::uint64_t available_bytes,
    const void* source, Rgba8MacroTiling mode, MacroRgba8Layout& out) {
    if (!source || !width || !height ||
        width > MaxNativeTextureDimension ||
        height > MaxNativeTextureDimension ||
        tiled_pitch < width || tiled_height < height ||
        tiled_pitch > MaxNativeTextureDimension ||
        tiled_height > MaxNativeTextureDimension ||
        tiled_pitch % MacroWidthRgba8 ||
        tiled_height % MacroHeightRgba8 ||
        bank_swizzle > 15 ||
        (mode != Rgba8MacroTiling::Display2DThin &&
         mode != Rgba8MacroTiling::Thin2DThin))
        return false;
    const auto required = std::uint64_t(tiled_pitch) * tiled_height * 4;
    const auto tight = std::uint64_t(width) * height * 4;
    if (required > MaxNativeTextureUploadBytes ||
        tight > MaxNativeTextureUploadBytes ||
        available_bytes < required)
        return false;
    out = {
        .visible = {width, height, width * 4, tight},
        .tiled_pitch = tiled_pitch,
        .tiled_height = tiled_height,
        .bank_swizzle = bank_swizzle,
        .required_input_bytes = required,
        .mode = mode,
    };
    return true;
}

// Byte address of a single RGBA8 pixel. For this *particular* configuration,
// tile_row/tile_column are zero (BANK_WIDTH=BANK_HEIGHT=1), the 256-byte
// microtile does not split, and the 256-byte pipe interleave has exactly
// three pipe bits followed by four bank bits. Each macro tile is 32 KiB.
// See tiling.comp ComputePipeFromCoord, ComputeBankFromCoord, and
// ComputeSurfaceAddrFromCoordMacroTiled for full generalized formulas.
inline std::uint64_t MacroPixelByteOffsetRgba8(
    std::uint32_t x, std::uint32_t y, const MacroRgba8Layout& layout) {
    const auto local_x = x / 8u;
    const auto local_y = y / 8u;
    const auto bit = [](std::uint32_t n, unsigned b) {
        return (n >> b) & 1u;
    };
    const auto x3 = bit(local_x,0), x4 = bit(local_x,1),
               x5 = bit(local_x,2);
    const auto y3 = bit(local_y,0), y4 = bit(local_y,1),
               y5 = bit(local_y,2);

    // PIPE_CONFIG=P8_32x32_16x16 (GCN constant 12).
    const auto p0 = x3 ^ y3 ^ x4;
    const auto p1 = x4 ^ y4;
    const auto p2 = x5 ^ y5;
    const auto pipe = p0 | (p1 << 1u) | (p2 << 2u);

    // Macro mode_1x1_16, 16 banks, bank width and height 1.
    // Bank tx = (x / 8) / 8, bank ty = y / 8.
    const auto bx = (x / 8u) / 8u;
    const auto by = y / 8u;
    const auto b0 = bit(bx,0) ^ bit(by,3);
    const auto b1 = bit(bx,1) ^ bit(by,2) ^ bit(by,3);
    const auto b2 = bit(bx,2) ^ bit(by,1);
    const auto b3 = bit(bx,3) ^ bit(by,0);
    const auto bank = ((b0 | (b1 << 1u) | (b2 << 2u) | (b3 << 3u)) ^
                       layout.bank_swizzle) & 15u;
    const auto mode = layout.mode == Rgba8MacroTiling::Display2DThin
        ? Rgba8Tiling::Display1DThin : Rgba8Tiling::Thin1DThin;
    const auto in_tile = PixelInMicroTile1DRgba8(x, y, mode);
    const auto macro_index = std::uint64_t(y / MacroHeightRgba8) *
                                 (layout.tiled_pitch / MacroWidthRgba8) +
                             x / MacroWidthRgba8;

    return macro_index * MacroBytesRgba8 +
           std::uint64_t(bank) * 2048u +
           std::uint64_t(pipe) * 256u +
           std::uint64_t(in_tile) * 4u;
}

// Precondition: Validate2DMacroRgba8 succeeded with the exact same source
// buffer. Destination contains at least visible.total_bytes bytes.
inline void Detile2DMacroRgba8(void* destination, const void* source,
                               const MacroRgba8Layout& layout) {
    auto* dst = static_cast<std::uint8_t*>(destination);
    const auto* src = static_cast<const std::uint8_t*>(source);
    for (std::uint32_t y = 0; y < layout.visible.height; ++y) {
        const auto out_row = std::uint64_t(y) * layout.visible.row_bytes;
        for (std::uint32_t x = 0; x < layout.visible.width; ++x) {
            const auto offset = MacroPixelByteOffsetRgba8(x, y, layout);
            std::memcpy(dst + out_row + std::uint64_t(x) * 4u,
                        src + offset, 4u);
        }
    }
}

} // namespace TlgNativeVk
