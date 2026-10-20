// SPDX-License-Identifier: GPL-2.0-or-later
// Pure C++ test for native Vulkan recording. No Vulkan device or headers.
#include "../gpu/shim/tlg_native_vk_draw.h"
#include <cassert>
#include <cstdint>
#include <vector>

struct Entry {
    int operation, pipeline;
    std::uint32_t count, instances, first_index, first_vertex, first_instance;
};
struct Recorder {
    std::vector<Entry>* events;
    void bindPipeline(int, int pipeline) {
        events->push_back({1,pipeline,0,0,0,0,0});
    }
    void draw(std::uint32_t count, std::uint32_t instances,
              std::uint32_t first_vertex, std::uint32_t first_instance) {
        events->push_back({2,0,count,instances,0,first_vertex,first_instance});
    }
    void drawIndexed(std::uint32_t count, std::uint32_t instances,
                     std::uint32_t first_index, std::int32_t vertex_offset,
                     std::uint32_t first_instance) {
        events->push_back({3,0,count,instances,first_index,
                           std::uint32_t(vertex_offset),first_instance});
    }
};
int main() {
    std::vector<Entry> events;
    Recorder recorder{&events};
    TlgNativeVk::Draw indexed{true,451,2,17,3,9};
    TlgNativeVk::EmitGraphicsDraw(recorder,0,21,0,indexed);
    assert(events.size()==2 && events[0].pipeline==21 && events[1].operation==3);
    assert(events[1].count==451 && events[1].instances==2);
    assert(events[1].first_index==9 && events[1].first_vertex==17 &&
           events[1].first_instance==3);
    events.clear();
    TlgNativeVk::Draw nonindexed{false,6,1,4,0,0};
    TlgNativeVk::EmitGraphicsDraw(recorder,0,12,18,nonindexed);
    assert(events.size()==4 && events[0].pipeline==12 && events[2].pipeline==18);
    assert(events[1].operation==2 && events[3].operation==2);
    assert(events[1].first_vertex==4 && events[3].count==6);
    return 0;
}
