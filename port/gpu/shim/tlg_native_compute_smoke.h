// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Opt-in, independent Vulkan compute proof: native SPIR-V -> VkComputePipeline
// -> VkBuffer storage descriptor -> vkCmdDispatch -> CPU readback.
//
// This pass NEVER references PS4 PM4 commands or game GPU registers and does
// not replace gameplay compute tasks. It exercises the core Vulkan facilities
// needed for native JsComputeContext execution once resources are mapped.
#include <memory>
namespace Vulkan { class Instance; class Scheduler; }
namespace TlgNativeVk {
class ComputeSmoke {
public:
    ComputeSmoke(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~ComputeSmoke();
    ComputeSmoke(const ComputeSmoke&) = delete;
    ComputeSmoke& operator=(const ComputeSmoke&) = delete;
    bool Init();
    bool RecordAndVerifyOnce();
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace TlgNativeVk
