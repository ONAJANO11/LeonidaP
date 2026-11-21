// Unit tests for the P4 descriptor-binding commands of the command-list IR.
//
// No GPU, no Windows objects and no game data: the IR records opaque heap ids and GPU handles, so the whole contract
// (limits, rejects, ordering, reset) is testable on the CPU. The replay validates the same limits; the case that
// needs Vulkan (scanning a bound table) is not in this test.

#include <cstdint>
#include <cstdio>
#include <variant>
#include <vector>

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
using leonida::SetComputeRootDescriptorTable;
using leonida::SetDescriptorHeaps;
using leonida::SetGraphicsRootDescriptorTable;

int test_descriptor_heaps_limits() {
    CommandList list;
    const std::uintptr_t one[] = {0x1000};
    const std::uintptr_t two[] = {0x1000, 0x2000};
    const std::uintptr_t duplicate[] = {0x1000, 0x1000};
    const std::uintptr_t zero[] = {0};
    const std::uintptr_t three[] = {1, 2, 3};

    EXPECT(list.set_descriptor_heaps({}) == false);  // no heap is not a heap set
    EXPECT(list.set_descriptor_heaps(zero) == false);
    EXPECT(list.set_descriptor_heaps(duplicate) == false);
    EXPECT(list.set_descriptor_heaps(three) == false);
    EXPECT(list.set_descriptor_heaps(one) == true);
    EXPECT(list.set_descriptor_heaps(two) == true);
    EXPECT(list.commands().size() == 2);

    const auto* first = std::get_if<SetDescriptorHeaps>(&list.commands()[0]);
    EXPECT(first && first->heaps.size() == 1 && first->heaps[0] == 0x1000);
    const auto* second = std::get_if<SetDescriptorHeaps>(&list.commands()[1]);
    EXPECT(second && second->heaps.size() == 2 && second->heaps[1] == 0x2000);
    return failures;
}

int test_root_descriptor_tables() {
    CommandList list;
    EXPECT(list.set_graphics_root_descriptor_table(0, 0x10000) == true);
    EXPECT(list.set_graphics_root_descriptor_table(63, 0x10000) == true);
    EXPECT(list.set_graphics_root_descriptor_table(64, 0x10000) == false);  // past D3D12's 64 root parameters
    EXPECT(list.set_graphics_root_descriptor_table(0, 0) == false);        // a zero GPU handle is invalid
    EXPECT(list.set_compute_root_descriptor_table(1, 0x20000) == true);
    EXPECT(list.set_compute_root_descriptor_table(64, 0x20000) == false);
    EXPECT(list.set_compute_root_descriptor_table(1, 0) == false);

    const auto* graphics = std::get_if<SetGraphicsRootDescriptorTable>(&list.commands()[0]);
    EXPECT(graphics && graphics->root_parameter == 0 && graphics->base_descriptor == 0x10000);
    const auto* compute = std::get_if<SetComputeRootDescriptorTable>(&list.commands()[2]);
    EXPECT(compute && compute->root_parameter == 1 && compute->base_descriptor == 0x20000);
    return failures;
}

int test_closed_list_rejects_bindings() {
    CommandList list;
    EXPECT(list.close());
    const std::uintptr_t heap[] = {0x1000};
    EXPECT(list.set_descriptor_heaps(heap) == false);
    EXPECT(list.set_graphics_root_descriptor_table(0, 0x1000) == false);
    EXPECT(list.set_compute_root_descriptor_table(0, 0x1000) == false);
    EXPECT(list.commands().empty());
    return failures;
}

int test_reset_clears_bindings() {
    CommandList list;
    const std::uintptr_t heap[] = {0x1000};
    EXPECT(list.set_descriptor_heaps(heap));
    EXPECT(list.set_graphics_root_descriptor_table(0, 0x1000));
    EXPECT(list.close());
    EXPECT(list.reset());
    EXPECT(list.commands().empty());
    EXPECT(list.set_descriptor_heaps(heap));  // usable again after Reset
    return failures;
}

int test_bindings_keep_issue_order() {
    CommandList list;
    const std::uintptr_t heap[] = {0x1000};
    EXPECT(list.set_descriptor_heaps(heap));
    EXPECT(list.set_graphics_root_descriptor_table(2, 0x3000));
    EXPECT(list.set_compute_root_descriptor_table(3, 0x4000));
    const auto commands = list.commands();
    EXPECT(commands.size() == 3);
    EXPECT(std::holds_alternative<SetDescriptorHeaps>(commands[0]));
    EXPECT(std::holds_alternative<SetGraphicsRootDescriptorTable>(commands[1]));
    EXPECT(std::holds_alternative<SetComputeRootDescriptorTable>(commands[2]));
    return failures;
}

}  // namespace

int main() {
    test_descriptor_heaps_limits();
    test_root_descriptor_tables();
    test_closed_list_rejects_bindings();
    test_reset_clears_bindings();
    test_bindings_keep_issue_order();

    if (failures) {
        std::fprintf(stderr, "command_bindings_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("command_bindings_test: all tests passed\n");
    return 0;
}
