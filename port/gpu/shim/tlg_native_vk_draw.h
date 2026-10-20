// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Native Vulkan graphics command encoder.
//
// This backend has NO dependency on the PS4 register file, PM4 opcodes, Gnmx,
// or the game's binary layout: it only consumes resolved Vulkan pipelines
// and high-level draw parameters. The old PM4 decoder currently supplies
// those parameters; the JsDeviceContext state backend will supply them once
// native shader and resource objects have been validated.
//
// Template form permits driver-free tests using a recording fake.
#include <cstdint>

namespace TlgNativeVk {
struct Draw {
    bool indexed = false;
    std::uint32_t vertex_or_index_count = 0;
    std::uint32_t instance_count = 1;
    std::uint32_t first_vertex = 0;
    std::uint32_t first_instance = 0;
    std::uint32_t first_index = 0;
};

template<class CommandBuffer, class Pipeline, class BindPoint>
void EmitGraphicsDraw(CommandBuffer buffer, BindPoint bind_point,
                      Pipeline pipeline, Pipeline square_pass, const Draw& draw) {
    const auto emit = [&] {
        if (draw.indexed) {
            buffer.drawIndexed(draw.vertex_or_index_count, draw.instance_count,
                               draw.first_index, static_cast<std::int32_t>(draw.first_vertex),
                               draw.first_instance);
        } else {
            buffer.draw(draw.vertex_or_index_count, draw.instance_count,
                        draw.first_vertex, draw.first_instance);
        }
    };
    buffer.bindPipeline(bind_point, pipeline);
    emit();
    if (square_pass) {
        buffer.bindPipeline(bind_point, square_pass);
        emit();
    }
}
} // namespace TlgNativeVk
