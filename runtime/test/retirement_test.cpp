// Unit tests for the fence-value retirement queue (P5).
//
// No Vulkan: the deleter is a closure, so the test counts releases and checks ordering, the pending count and the
// value boundary (an item retired at value V is released by collect(V), not by collect(V-1)).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retirement.h"

namespace {

int failures = 0;

#define EXPECT(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

int test_collect_releases_only_completed_values() {
    leonida::RetirementQueue queue;
    std::vector<int> released;
    queue.retire(5, [&] { released.push_back(1); });
    queue.retire(7, [&] { released.push_back(2); });
    queue.retire(9, [&] { released.push_back(3); });
    EXPECT(queue.pending() == 3);

    EXPECT(queue.collect(4) == 0);
    EXPECT(released.empty());
    EXPECT(queue.pending() == 3);

    EXPECT(queue.collect(5) == 1);  // the boundary value is included
    EXPECT(released.size() == 1 && released[0] == 1);
    EXPECT(queue.pending() == 2);

    EXPECT(queue.collect(8) == 1);
    EXPECT(released.size() == 2 && released[1] == 2);
    EXPECT(queue.collect(100) == 1);
    EXPECT(released.size() == 3 && queue.pending() == 0);
    return failures;
}

int test_same_value_items_keep_their_order() {
    leonida::RetirementQueue queue;
    std::vector<int> order;
    for (int i = 0; i < 4; ++i) queue.retire(3, [&order, i] { order.push_back(i); });
    EXPECT(queue.pending_at(3) == 4);
    EXPECT(queue.collect(3) == 4);
    EXPECT(order == std::vector<int>({0, 1, 2, 3}));
    return failures;
}

int test_zero_value_waits_for_the_next_collect() {
    leonida::RetirementQueue queue;
    bool released = false;
    queue.retire(0, [&] { released = true; });
    EXPECT(!released);  // not destroyed on the spot
    EXPECT(queue.collect(0) == 1);
    EXPECT(released);
    return failures;
}

int test_empty_deleter_is_ignored() {
    leonida::RetirementQueue queue;
    queue.retire(1, {});
    EXPECT(queue.pending() == 0);
    return failures;
}

}  // namespace

int main() {
    test_collect_releases_only_completed_values();
    test_same_value_items_keep_their_order();
    test_zero_value_waits_for_the_next_collect();
    test_empty_deleter_is_ignored();

    if (failures) {
        std::fprintf(stderr, "retirement_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("retirement_test: all tests passed\n");
    return 0;
}
