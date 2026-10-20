// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Native CPU-side detiler for the EXACT 1D thin modes in AMD GCN/PS4.
//
// The 8x8 microtile permutation below matches the existing
// video_core/host_shaders/tiling.comp ComputePixelIndexWithinMicroTile()
// with BITS_PER_PIXEL=32 and MICRO_TILE_THICKNESS=NUM_SAMPLES=1.
// This is NOT a macro-tile, bank-swizzle, compressed, depth or mip decoder.
// The caller supplies verified pitch/padded-height from Js resource metadata.
#include "tlg_native_texture_layout.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace TlgNativeVk {

enum class Rgba8Tiling : std::uint32_t {
    Display1DThin = 9,  // AmdGpu::TileMode::Display1DThin
    Thin1DThin = 13,    // AmdGpu::TileMode::Thin1DThin
};

struct DetileRgba8Layout {
    LinearRgba8 visible{};
    std::uint32_t tiled_pitch = 0;     // pixels; multiple of 8
    std::uint32_t tiled_height = 0;    // pixels; multiple of 8
    std::uint64_t required_input_bytes = 0;
    Rgba8Tiling tiling = Rgba8Tiling::Thin1DThin;
};

inline bool Validate1DTiledRgba8(
    std::uint32_t visible_width, std::uint32_t visible_height,
    std::uint32_t tiled_pitch, std::uint32_t tiled_height,
    std::uint64_t available_bytes, const void* input, Rgba8Tiling mode,
    DetileRgba8Layout& out) {
    if (!input || !visible_width || !visible_height ||
        visible_width > MaxNativeTextureDimension ||
        visible_height > MaxNativeTextureDimension ||
        tiled_pitch < visible_width || tiled_height < visible_height ||
        tiled_pitch > MaxNativeTextureDimension ||
        tiled_height > MaxNativeTextureDimension ||
        (tiled_pitch & 7u) != 0 || (tiled_height & 7u) != 0 ||
        (mode != Rgba8Tiling::Display1DThin &&
         mode != Rgba8Tiling::Thin1DThin))
        return false;
    // Only a single 32-bit, one-sample, one-slice 1D thin tiled surface.
    // Input includes whole 8x8 microtiles, including padding.
    const auto required = std::uint64_t(tiled_pitch) * tiled_height * 4u;
    const auto visible_bytes = std::uint64_t(visible_width) * visible_height * 4u;
    if (required > MaxNativeTextureUploadBytes ||
        visible_bytes > MaxNativeTextureUploadBytes ||
        available_bytes < required)
        return false;
    out = {
        .visible = {
            .width = visible_width,
            .height = visible_height,
            .row_bytes = visible_width * 4u,
            .total_bytes = visible_bytes,
        },
        .tiled_pitch = tiled_pitch,
        .tiled_height = tiled_height,
        .required_input_bytes = required,
        .tiling = mode,
    };
    return true;
}

// Returns pixel index within the 8x8 32bpp microtile. These are the
// precise bit assignments from tiling.comp for Thin and Display modes.
inline std::uint32_t PixelInMicroTile1DRgba8(
    std::uint32_t x, std::uint32_t y, Rgba8Tiling mode) {
    const auto x0 = x & 1u, x1 = (x >> 1u) & 1u, x2 = (x >> 2u) & 1u;
    const auto y0 = y & 1u, y1 = (y >> 1u) & 1u, y2 = (y >> 2u) & 1u;
    if (mode == Rgba8Tiling::Display1DThin)
        return x0 | (x1 << 1u) | (y0 << 2u) | (x2 << 3u) |
               (y1 << 4u) | (y2 << 5u);
    return x0 | (y0 << 1u) | (x1 << 2u) | (y1 << 3u) |
           (x2 << 4u) | (y2 << 5u);
}

// Precondition: Validate1DTiledRgba8 succeeded with this input/layout,
// destination contains at least layout.visible.total_bytes bytes.
// The result is tightly packed linear RGBA8, suitable for the existing
// independent Vulkan CreateSampledTextureRGBA8() upload path.
inline void Detile1DRgba8(void* destination, const void* source,
                           const DetileRgba8Layout& layout) {
    auto* dst = static_cast<std::uint8_t*>(destination);
    const auto* src = static_cast<const std::uint8_t*>(source);
    const auto microtiles_per_row = layout.tiled_pitch / 8u;
    for (std::uint32_t y = 0; y < layout.visible.height; ++y) {
        const auto tile_row = std::uint64_t(y / 8u) * microtiles_per_row;
        const auto output_row = std::uint64_t(y) * layout.visible.row_bytes;
        for (std::uint32_t x = 0; x < layout.visible.width; ++x) {
            const auto tile = tile_row + x / 8u;
            const auto within = PixelInMicroTile1DRgba8(x, y, layout.tiling);
            const auto source_byte = (tile * 64u + within) * 4u;
            std::memcpy(dst + output_row + std::uint64_t(x) * 4u,
                        src + source_byte, 4u);
        }
    }
}

} // namespace TlgNativeVk
