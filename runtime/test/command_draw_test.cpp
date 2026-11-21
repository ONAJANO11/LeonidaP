// Unit tests for the PSO/root-signature/draw commands of the command-list IR (milestone M1).
//
// The IR records opaque object ids and counts; nothing runs on a GPU here. The replay validates the same rules, so a
// bad draw fails at prepare() instead of reaching Vulkan.

#include <cstdint>
#include <cstdio>
#include <variant>

#include "command_list.h"

namespace {

int failures = 0;

#define EXPECT(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

using leonida::CommandList;
using leonida::DrawIndexedInstanced;
using leonida::DrawInstanced;
using leonida::SetGraphicsRootSignature;
using leonida::SetPipelineState;

int test_pipeline_and_signatures() {
    CommandList list;
    EXPECT(!list.set_pipeline_state(0));  // a zero id is not a pipeline
    EXPECT(list.set_pipeline_state(0x1000));
    EXPECT(!list.set_graphics_root_signature(0));
    EXPECT(list.set_graphics_root_signature(0x2000));
    EXPECT(list.set_compute_root_signature(0x3000));

    EXPECT(std::holds_alternative<SetPipelineState>(list.commands()[0]));
    EXPECT(std::holds_alternative<SetGraphicsRootSignature>(list.commands()[1]));
    EXPECT(std::holds_alternative<leonida::SetComputeRootSignature>(list.commands()[2]));
    return failures;
}

int test_draw_validation() {
    CommandList list;
    EXPECT(!list.draw_instanced(0));           // no vertices
    EXPECT(!list.draw_instanced(3, 0));        // no instances
    EXPECT(list.draw_instanced(3));            // one instance, first vertex 0
    EXPECT(!list.draw_indexed_instanced(0));
    EXPECT(!list.draw_indexed_instanced(6, 0));
    EXPECT(list.draw_indexed_instanced(6, 2, 3, -1, 4));  // a negative base vertex is legal

    const auto* draw = std::get_if<DrawInstanced>(&list.commands()[0]);
    EXPECT(draw && draw->vertex_count_per_instance == 3 && draw->instance_count == 1);
    const auto* indexed = std::get_if<DrawIndexedInstanced>(&list.commands()[1]);
    EXPECT(indexed && indexed->index_count_per_instance == 6 && indexed->instance_count == 2 &&
           indexed->start_index == 3 && indexed->base_vertex == -1 && indexed->start_instance == 4);
    return failures;
}

int test_closed_list_rejects_draws() {
    CommandList list;
    EXPECT(list.set_pipeline_state(0x1000));
    EXPECT(list.close());
    EXPECT(!list.draw_instanced(3));
    EXPECT(!list.draw_indexed_instanced(3));
    EXPECT(!list.set_pipeline_state(0x2000));
    return failures;
}

int test_reset_allows_recording_again() {
    CommandList list;
    EXPECT(list.draw_instanced(3));
    EXPECT(list.close());
    EXPECT(list.reset());
    EXPECT(list.commands().empty());
    EXPECT(list.draw_instanced(3));
    return failures;
}

}  // namespace

int main() {
    test_pipeline_and_signatures();
    test_draw_validation();
    test_closed_list_rejects_draws();
    test_reset_allows_recording_again();

    if (failures) {
        std::fprintf(stderr, "command_draw_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("command_draw_test: all tests passed\n");
    return 0;
}
