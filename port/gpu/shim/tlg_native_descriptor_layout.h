// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Native Js shader descriptor reflection contract, independent of PM4/Vulkan.
// Binding numbers are absolute SPIR-V set-0 bindings emitted by the GCN
// compiler. A verified guest resource view must be supplied separately; a
// descriptor *layout* is never proof that a texture/buffer is ready for a draw.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace TlgNativeVk {
enum class NativeDescriptorKind : std::uint32_t {
    StorageBuffer = 1, SampledImage = 2, StorageImage = 3, Sampler = 4,
};
struct NativeDescriptorBinding {
    std::uint32_t binding = 0;
    std::uint32_t descriptor_count = 1;
    NativeDescriptorKind kind = NativeDescriptorKind::StorageBuffer;
    bool operator==(const NativeDescriptorBinding&) const = default;
};

constexpr std::uint32_t MaxNativeDescriptorSlots = 64;

inline bool IsNativeDescriptorKind(NativeDescriptorKind kind) {
    switch (kind) {
    case NativeDescriptorKind::StorageBuffer:
    case NativeDescriptorKind::SampledImage:
    case NativeDescriptorKind::StorageImage:
    case NativeDescriptorKind::Sampler:
        return true;
    default:
        return false;
    }
}

// Explicit first_binding permits a fragment stage to begin immediately after
// the vertex stage's last binding. Never permit silent gaps or duplicate slots.
inline bool AppendNativeDescriptor(std::vector<NativeDescriptorBinding>& layout,
                                   std::uint32_t first_binding,
                                   NativeDescriptorKind kind,
                                   std::uint32_t descriptor_count = 1) {
    if (!IsNativeDescriptorKind(kind) || !descriptor_count ||
        first_binding > MaxNativeDescriptorSlots ||
        descriptor_count > MaxNativeDescriptorSlots)
        return false;
    std::uint32_t at = first_binding;
    if (!layout.empty()) {
        const auto& previous = layout.back();
        if (previous.binding < first_binding ||
            previous.binding > MaxNativeDescriptorSlots ||
            previous.descriptor_count > MaxNativeDescriptorSlots - previous.binding)
            return false;
        at = previous.binding + previous.descriptor_count;
    }
    if (at > MaxNativeDescriptorSlots ||
        descriptor_count > MaxNativeDescriptorSlots - at)
        return false;
    layout.push_back({at, descriptor_count, kind});
    return true;
}

inline bool ValidNativeDescriptorLayout(
    std::span<const NativeDescriptorBinding> layout,
    std::uint32_t first_binding = 0) {
    if (first_binding > MaxNativeDescriptorSlots) return false;
    std::uint32_t next = first_binding;
    for (const auto& descriptor : layout) {
        if (!IsNativeDescriptorKind(descriptor.kind) ||
            descriptor.binding != next || !descriptor.descriptor_count ||
            descriptor.descriptor_count > MaxNativeDescriptorSlots - next)
            return false;
        next += descriptor.descriptor_count;
    }
    return true;
}

inline bool MatchNativeDescriptorLayout(
    std::span<const NativeDescriptorBinding> expected,
    std::span<const NativeDescriptorBinding> reflected,
    std::uint32_t first_binding = 0) {
    if (!ValidNativeDescriptorLayout(expected, first_binding) ||
        !ValidNativeDescriptorLayout(reflected, first_binding) ||
        expected.size() != reflected.size())
        return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!(expected[i] == reflected[i])) return false;
    }
    return true;
}
} // namespace TlgNativeVk
