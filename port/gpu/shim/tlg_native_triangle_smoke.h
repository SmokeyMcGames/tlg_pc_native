// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Full Vulkan-native (no PM4, no GCN) offscreen graphics test pass.
// Uses a synthetic triangle, NOT the Last Guardian's game shaders or scene.
#include <memory>
namespace Vulkan { class Instance; class Scheduler; }
namespace TlgNativeVk {
class DeviceResources;
class ResourceRegistry;
class TriangleSmoke {
public:
    TriangleSmoke(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                  DeviceResources& resources, ResourceRegistry& registry,
                  bool sampled_texture_test = false);
    ~TriangleSmoke();
    TriangleSmoke(const TriangleSmoke&) = delete;
    TriangleSmoke& operator=(const TriangleSmoke&) = delete;

    bool Init();
    bool RecordOnce();
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace TlgNativeVk
