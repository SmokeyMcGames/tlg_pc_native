// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Immutable descriptor assignments from verified high-level Js resources.
// Do not infer a GPU handle from a guest pointer. Every element carries the
// exact native registry generation observed when the game resource was bound.
#include "tlg_native_descriptor_layout.h"
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace TlgNativeVk {
struct NativeGuestDescriptorElement {
    std::uint64_t guest_id = 0;
    std::uint64_t generation = 0;
    bool operator==(const NativeGuestDescriptorElement&) const = default;
};
struct NativeGuestDescriptorBinding {
    std::uint32_t binding = 0;
    NativeDescriptorKind kind = NativeDescriptorKind::StorageBuffer;
    std::vector<NativeGuestDescriptorElement> resources{};
};

// Complete, ordered, generation-checked set-0 contract. One Vulkan binding
// can have multiple elements (e.g. an array of sampled images), but every
// slot must have an independently registered resource with a known lifetime.
inline bool ValidateNativeGuestDescriptors(
    std::span<const NativeDescriptorBinding> expected,
    std::span<const NativeGuestDescriptorBinding> assigned) {
    if (!ValidNativeDescriptorLayout(expected) ||
        expected.size() != assigned.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& spec = expected[i];
        const auto& actual = assigned[i];
        if (actual.binding != spec.binding || actual.kind != spec.kind ||
            actual.resources.size() != spec.descriptor_count ||
            std::any_of(actual.resources.begin(), actual.resources.end(),
                        [](const NativeGuestDescriptorElement& item) {
                            return !item.guest_id || !item.generation;
                        })) return false;
    }
    return true;
}
} // namespace TlgNativeVk
