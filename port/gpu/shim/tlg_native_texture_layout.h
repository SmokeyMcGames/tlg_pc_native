// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// PM4-independent texture upload preflight. Only linear RGBA8, one 2D
// mip level, tightly staged for Vulkan. PS4 tiled/swizzled, compressed,
// depth, mipmapped, cube and array formats MUST use separate verified paths.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace TlgNativeVk {
struct LinearRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_bytes = 0;
    std::uint64_t total_bytes = 0;
};

static constexpr std::uint32_t MaxNativeTextureDimension = 8192;
static constexpr std::uint64_t MaxNativeTextureUploadBytes = 256ull * 1024 * 1024;

inline bool ValidateLinearRgba8(std::uint32_t width, std::uint32_t height,
                               std::uint64_t source_row_pitch,
                               std::uint64_t available_bytes,
                               const void* source, LinearRgba8& out) {
    if (!source || !width || !height ||
        width > MaxNativeTextureDimension ||
        height > MaxNativeTextureDimension)
        return false;
    const std::uint64_t row = std::uint64_t(width) * 4;
    const std::uint64_t tight = row * height;
    if (tight > MaxNativeTextureUploadBytes ||
        source_row_pitch < row ||
        source_row_pitch > MaxNativeTextureUploadBytes)
        return false;
    // Evaluate (height - 1) * pitch only AFTER ensuring its maximum cannot
    // exceed the budget; prevents overflow and truncated-source reads.
    if (std::uint64_t(height - 1) >
        (MaxNativeTextureUploadBytes - row) / source_row_pitch)
        return false;
    const std::uint64_t needed = std::uint64_t(height - 1) * source_row_pitch + row;
    if (needed > available_bytes)
        return false;
    out = {
        .width = width,
        .height = height,
        .row_bytes = static_cast<std::uint32_t>(row),
        .total_bytes = tight,
    };
    return true;
}

// Precondition: destination has out.total_bytes bytes; source passed the
// matching ValidateLinearRgba8 check. No tiling or format conversion.
inline void CopyRgba8Rows(void* destination, const void* source,
                          const LinearRgba8& out,
                          std::uint64_t source_row_pitch) {
    auto* dst = static_cast<std::uint8_t*>(destination);
    const auto* src = static_cast<const std::uint8_t*>(source);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        std::memcpy(dst + std::uint64_t(y) * out.row_bytes,
                    src + std::uint64_t(y) * source_row_pitch,
                    out.row_bytes);
    }
}

enum class NativeTexturePhase : std::uint8_t {
    Invalid, Allocated, UploadRecorded, ShaderReadable, Retired
};

class NativeTextureTransition {
public:
    NativeTexturePhase Phase() const { return phase_; }
    bool Allocate() {
        if (phase_ != NativeTexturePhase::Invalid &&
            phase_ != NativeTexturePhase::Retired)
            return false;
        phase_ = NativeTexturePhase::Allocated;
        ++generation_;
        return true;
    }
    bool BeginUpload() {
        if (phase_ != NativeTexturePhase::Allocated) return false;
        phase_ = NativeTexturePhase::UploadRecorded;
        return true;
    }
    bool MarkShaderReadable() {
        if (phase_ != NativeTexturePhase::UploadRecorded) return false;
        phase_ = NativeTexturePhase::ShaderReadable;
        return true;
    }
    bool Ready(std::uint64_t expected_generation) const {
        return phase_ == NativeTexturePhase::ShaderReadable &&
               expected_generation == generation_;
    }
    void Retire() { phase_ = NativeTexturePhase::Retired; }
    std::uint64_t Generation() const { return generation_; }
private:
    NativeTexturePhase phase_ = NativeTexturePhase::Invalid;
    std::uint64_t generation_ = 0;
};
} // namespace TlgNativeVk
