// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Reusable, driver/profile/version-specific native SPIR-V shader cache.
//
// This stores only SPIR-V binaries produced from VERIFIED Js-level shader
// inputs; it never reads PS4 PM4 packets. Future Vulkan-native pipelines may
// retain a shared immutable Module across worker and render threads.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>
#include "tlg_native_descriptor_layout.h"

namespace TlgNativeVk {

enum class NativeShaderStage : std::uint32_t {
    Vertex = 1, Fragment = 2, Compute = 3,
};
struct NativeShaderKey {
    std::uint64_t guest_id = 0;            // opaque game shader object
    std::uint64_t game_binary_hash = 0;    // verified game version
    std::uint64_t code_hash = 0;           // full source-code identity
    std::uint64_t specialization_hash = 0; // user-data, shader variants, RT layout
    std::uint64_t resource_layout_hash = 0;
    std::uint64_t compiler_revision = 0;   // IR/SPIR-V recompiler version
    std::uint32_t vk_vendor = 0;
    std::uint32_t vk_device = 0;
    std::uint32_t vk_driver = 0;
    NativeShaderStage stage{};
    bool operator==(const NativeShaderKey&) const = default;
};
struct NativeShaderModule {
    NativeShaderKey key;
    std::uint64_t generation{};
    std::vector<std::uint32_t> spirv;
    std::vector<NativeDescriptorBinding> descriptors;
    std::uint32_t first_binding = 0;
};

// Cache reuse must link the original absolute descriptor numbers, not
// accidentally restart the fragment stage at zero. Pure C++ validation,
// shared by Vulkan pipeline creation and the driver-free regression tests.
inline bool AssembleCachedGraphicsDescriptors(
    const NativeShaderModule& vertex,
    const NativeShaderModule& fragment,
    std::vector<NativeDescriptorBinding>& out) {
    out.clear();
    if (vertex.key.stage != NativeShaderStage::Vertex ||
        fragment.key.stage != NativeShaderStage::Fragment ||
        vertex.first_binding != 0 ||
        !ValidNativeDescriptorLayout(vertex.descriptors, 0))
        return false;
    std::uint32_t next = 0;
    if (!vertex.descriptors.empty()) {
        const auto& last = vertex.descriptors.back();
        next = last.binding + last.descriptor_count;
    }
    // Shader modules contain SPIR-V with fixed set/binding decorations,
    // so the stage's recorded starting slot must be exact, even if it
    // does not itself consume any descriptor.
    if (fragment.first_binding != next ||
        !ValidNativeDescriptorLayout(fragment.descriptors, next))
        return false;
    out = vertex.descriptors;
    out.insert(out.end(), fragment.descriptors.begin(),
               fragment.descriptors.end());
    return ValidNativeDescriptorLayout(out);
}


class NativeShaderRegistry {
public:
    explicit NativeShaderRegistry(std::size_t max_bytes = 128u * 1024u * 1024u)
        : max_bytes_{max_bytes} {}

    static bool IsCompleteKey(const NativeShaderKey& key) {
        const auto stage = key.stage;
        return key.guest_id && key.game_binary_hash && key.code_hash &&
               key.specialization_hash && key.resource_layout_hash &&
               key.compiler_revision && key.vk_vendor && key.vk_device &&
               key.vk_driver &&
               (stage == NativeShaderStage::Vertex ||
                stage == NativeShaderStage::Fragment ||
                stage == NativeShaderStage::Compute);
    }

    // Atomically replaces earlier shaders at this guest object address.
    // No cache entries are accepted from a guessed driver profile or
    // incomplete shader/resource specialization. Vulkan device handles
    // are not created here.
    bool Publish(const NativeShaderKey& key, std::span<const std::uint32_t> spirv,
                 bool descriptors_resolved,
                 std::span<const NativeDescriptorBinding> descriptors = {},
                 std::uint32_t first_binding = 0) {
        if (!descriptors_resolved || !IsCompleteKey(key) ||
            spirv.size() < 5 || spirv.size() > (1u << 20) ||
            spirv[0] != 0x07230203u ||
            !ValidNativeDescriptorLayout(descriptors, first_binding) ||
            (key.stage == NativeShaderStage::Compute && first_binding != 0))
            return false;
        // Descriptor metadata consumes part of the cache budget too.
        const std::size_t bytes = spirv.size_bytes() + descriptors.size_bytes();
        if (bytes > max_bytes_) return false;
        auto next = std::make_shared<NativeShaderModule>();
        next->key = key;
        next->spirv.assign(spirv.begin(), spirv.end());
        next->descriptors.assign(descriptors.begin(), descriptors.end());
        next->first_binding = first_binding;
        std::scoped_lock lock{mutex_};
        const auto old = modules_.find(key.guest_id);
        const std::size_t reusable_bytes =
            old == modules_.end() ? 0 :
                old->second->spirv.size() * sizeof(std::uint32_t) +
                old->second->descriptors.size() * sizeof(NativeDescriptorBinding);
        // Reject the new version BEFORE modifying the previous one.
        // A failed cache insertion must leave existing working state intact.
        if (bytes > max_bytes_ - (occupied_bytes_ - reusable_bytes)) return false;
        if (old != modules_.end()) {
            occupied_bytes_ -= reusable_bytes;
            modules_.erase(old);
        }
        next->generation = ++next_generation_;
        occupied_bytes_ += bytes;
        modules_[key.guest_id] = std::move(next);
        return true;
    }

    std::shared_ptr<const NativeShaderModule>
    Find(const NativeShaderKey& exact_key) const {
        std::scoped_lock lock{mutex_};
        const auto it = modules_.find(exact_key.guest_id);
        if (it == modules_.end() || it->second->key != exact_key) return {};
        return it->second;
    }

    void Retire(std::uint64_t guest_id) {
        std::scoped_lock lock{mutex_};
        if (auto it = modules_.find(guest_id); it != modules_.end()) {
            occupied_bytes_ -= it->second->spirv.size() * sizeof(std::uint32_t) +
                               it->second->descriptors.size() * sizeof(NativeDescriptorBinding);
            modules_.erase(it);
        }
    }

    std::size_t ResidentBytes() const {
        std::scoped_lock lock{mutex_};
        return occupied_bytes_;
    }

private:
    mutable std::mutex mutex_;
    std::size_t max_bytes_;
    std::size_t occupied_bytes_ = 0;
    std::uint64_t next_generation_ = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<NativeShaderModule>> modules_;
};

} // namespace TlgNativeVk
