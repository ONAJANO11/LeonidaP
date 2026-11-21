// Unit tests for the bindless index allocator (P3 of docs/d3d12_vulkan_analysis.md).
//
// No GPU and no game data: the contract is the index 0 fallback, unique/sticky indices, the free list, and a full
// pool returning 0. The keys are T#/S# words; their content is irrelevant to the allocator.

#include <cstdint>
#include <cstdio>
#include <set>

#include "bindless_allocator.h"

namespace {

int failures = 0;

#define EXPECT(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

leonida::DescriptorKey key(std::uint32_t seed) {
    leonida::DescriptorKey out{};
    for (std::uint32_t i = 0; i < 8; ++i) out[i] = seed * 0x9E3779B1u + i;
    return out;
}

int test_indices_start_at_one_and_are_unique() {
    leonida::BindlessAllocator pool(4);
    std::set<std::uint32_t> seen;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t index = pool.allocate();
        EXPECT(index != 0);
        EXPECT(seen.insert(index).second);
        EXPECT(pool.in_use(index));
    }
    EXPECT(pool.allocated() == 4);
    EXPECT(pool.free_slots() == 0);
    EXPECT(pool.allocate() == 0);  // full
    return failures;
}

int test_ensure_is_sticky_and_find_reports_misses() {
    leonida::BindlessAllocator pool(8);
    const leonida::DescriptorKey a = key(1), b = key(2);
    EXPECT(pool.find(a) == 0);
    const std::uint32_t ia = pool.ensure(a);
    EXPECT(ia != 0);
    EXPECT(pool.ensure(a) == ia);  // same key, same index
    const std::uint32_t ib = pool.ensure(b);
    EXPECT(ib != 0 && ib != ia);
    EXPECT(pool.find(a) == ia && pool.find(b) == ib);
    EXPECT(pool.allocated() == 2);
    return failures;
}

int test_release_returns_the_slot_to_the_free_list() {
    leonida::BindlessAllocator pool(4);
    const leonida::DescriptorKey a = key(3);
    const std::uint32_t index = pool.ensure(a);
    EXPECT(pool.release(index));
    EXPECT(!pool.in_use(index));
    EXPECT(pool.find(a) == 0);  // a released key misses again
    EXPECT(pool.free_slots() == 4);
    // The next allocation reuses the released slot (LIFO), never the fallback.
    EXPECT(pool.allocate() == index);
    return failures;
}

int test_release_rejects_bad_indices() {
    leonida::BindlessAllocator pool(2);
    EXPECT(!pool.release(0));    // the fallback is never allocatable
    EXPECT(!pool.release(3));    // past the capacity
    EXPECT(!pool.release(1));    // never allocated
    const std::uint32_t index = pool.allocate();
    EXPECT(pool.release(index));
    EXPECT(!pool.release(index));  // a double release is not a second free slot
    return failures;
}

int test_ensure_reports_a_full_pool() {
    leonida::BindlessAllocator pool(2);
    EXPECT(pool.ensure(key(10)) != 0);
    EXPECT(pool.ensure(key(11)) != 0);
    EXPECT(pool.ensure(key(12)) == 0);
    EXPECT(pool.find(key(12)) == 0);
    // Freeing one slot lets exactly one more key in.
    EXPECT(pool.release(1));
    EXPECT(pool.ensure(key(12)) == 1);
    EXPECT(pool.ensure(key(13)) == 0);
    return failures;
}

int test_zero_capacity_is_clamped_to_one() {
    leonida::BindlessAllocator pool(0);
    EXPECT(pool.capacity() == 1);
    EXPECT(pool.allocate() == 1);
    EXPECT(pool.allocate() == 0);
    return failures;
}

}  // namespace

int main() {
    test_indices_start_at_one_and_are_unique();
    test_ensure_is_sticky_and_find_reports_misses();
    test_release_returns_the_slot_to_the_free_list();
    test_release_rejects_bad_indices();
    test_ensure_reports_a_full_pool();
    test_zero_capacity_is_clamped_to_one();

    if (failures) {
        std::fprintf(stderr, "bindless_allocator_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("bindless_allocator_test: all tests passed\n");
    return 0;
}
