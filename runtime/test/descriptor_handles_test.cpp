// Unit tests for the descriptor-handle arithmetic and dirty ranges (P0/P1 of docs/d3d12_vulkan_analysis.md).
//
// No Vulkan, no GPU and no game data: the contract is (handle - base) / stride, the range a run of slots covers,
// and the half-open dirty range the bindless scan reads. Run it directly or through ctest.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "descriptor_handles.h"
#include "descriptor_sizes.h"

namespace {

int failures = 0;

#define EXPECT(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

using rt::d3d12::DirtyRange;
using rt::d3d12::merge;
using rt::d3d12::slot_index;
using rt::d3d12::slot_range;
using rt::d3d12::slot_run_bytes;

int test_slot_index_maps_inside_the_heap() {
    constexpr std::uint32_t stride = rt::d3d12::descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    constexpr std::uintptr_t base = 0x1000;
    constexpr std::uint32_t count = 8;
    std::uint32_t slot = 0xFFFF;

    EXPECT(slot_index(base, base, count, stride, &slot) && slot == 0);
    EXPECT(slot_index(base + 3 * stride, base, count, stride, &slot) && slot == 3);
    EXPECT(slot_index(base + 7 * stride, base, count, stride, &slot) && slot == 7);  // the last slot
    EXPECT(slot_index(base + 8 * stride, base, count, stride, &slot) == false);      // one past the end
    EXPECT(slot_index(base - 1, base, count, stride, &slot) == false);               // below the heap
    EXPECT(slot_index(base + stride / 2, base, count, stride, &slot) == false);      // unaligned
    EXPECT(slot_index(base, base, 0, stride, &slot) == false);                       // empty heap
    EXPECT(slot_index(base, base, count, 0, &slot) == false);                        // empty stride
    return failures;
}

int test_slot_index_works_for_every_heap_type() {
    constexpr std::uintptr_t base = 0x2000;
    for (UINT t = 0; t < static_cast<UINT>(D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES); ++t) {
        const std::uint32_t stride = rt::d3d12::descriptor_slot_bytes(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(t));
        std::uint32_t slot = 0xFFFF;
        EXPECT(slot_index(base + 2 * stride, base, 4, stride, &slot) && slot == 2);
    }
    return failures;
}

int test_slot_run_bytes_does_not_overflow() {
    std::size_t bytes = 0;
    EXPECT(slot_run_bytes(4, 32, &bytes) && bytes == 128);
    EXPECT(slot_run_bytes(0, 32, &bytes) && bytes == 0);
    EXPECT(slot_run_bytes(4, 0, &bytes) && bytes == 0);
    // 32-bit operands only overflow a 32-bit size_t; on 64-bit the product fits and must be exact.
    const std::uint64_t product = std::uint64_t(0xFFFFFFFFu) * 0xFFFFFFFFu;
    if (std::numeric_limits<std::size_t>::max() < product) {
        EXPECT(slot_run_bytes(0xFFFFFFFFu, 0xFFFFFFFFu, &bytes) == false);
    } else {
        EXPECT(slot_run_bytes(0xFFFFFFFFu, 0xFFFFFFFFu, &bytes) && bytes == product);
    }
    return failures;
}

int test_dirty_ranges_merge() {
    EXPECT(DirtyRange{}.empty());
    EXPECT(merge(DirtyRange{}, slot_range(2, 3)).first == 2);  // empty is the identity
    EXPECT(merge(slot_range(2, 3), DirtyRange{}).count() == 3);

    const DirtyRange a = slot_range(2, 3);   // [2, 5)
    const DirtyRange b = slot_range(4, 4);   // [4, 8)
    const DirtyRange m = merge(a, b);
    EXPECT(m.first == 2 && m.end == 8 && m.count() == 6);

    const DirtyRange disjoint = merge(slot_range(10, 2), slot_range(0, 1));
    EXPECT(disjoint.first == 0 && disjoint.end == 12 && disjoint.count() == 12);

    EXPECT(slot_range(0, 0).empty());
    EXPECT(slot_range(7, 1).count() == 1);
    return failures;
}

int test_no_slot_overlap_across_a_heap() {
    // Every address in a heap belongs to at most one slot: walk a small heap and ensure exactly one slot claims
    // each aligned handle, which is what Create*View relies on when it writes a descriptor.
    constexpr std::uint32_t stride = rt::d3d12::descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    constexpr std::uintptr_t base = 0x3000;
    constexpr std::uint32_t count = 16;
    std::vector<int> claims(count, 0);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t slot = 0xFFFF;
        if (slot_index(base + i * stride, base, count, stride, &slot)) ++claims[slot];
    }
    for (int c : claims) EXPECT(c == 1);
    return failures;
}

int test_copy_between_two_heaps() {
    // P1: CopyDescriptors moves slots between two heaps. Both heaps exist as separate host shadows; the copy is
    // clamped to each heap and the destination stride is the destination heap's.
    const std::uint32_t stride = rt::d3d12::descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::vector<std::uint8_t> destination(8 * stride, 0), source(6 * stride, 0);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);

    // Copy two slots from source[1] to destination[5].
    EXPECT(rt::d3d12::copy_slots_between(destination.data(), 8, 5, source.data(), 6, 1, stride, 2) == 2);
    for (std::uint32_t i = 0; i < 2; ++i) {
        EXPECT(std::memcmp(destination.data() + (5 + i) * stride, source.data() + (1 + i) * stride, stride) == 0);
    }
    EXPECT(std::memcmp(destination.data() + 4 * stride, std::vector<std::uint8_t>(stride, 0).data(), stride) == 0);

    // Asking for more than the source has copies only what fits.
    EXPECT(rt::d3d12::copy_slots_between(destination.data(), 8, 0, source.data(), 6, 4, stride, 5) == 2);
    EXPECT(rt::d3d12::copy_slots_between(destination.data(), 8, 6, source.data(), 6, 0, stride, 9) == 2);

    // Out-of-range and empty requests copy nothing.
    EXPECT(rt::d3d12::copy_slots_between(destination.data(), 8, 8, source.data(), 6, 0, stride, 1) == 0);
    EXPECT(rt::d3d12::copy_slots_between(destination.data(), 8, 0, source.data(), 6, 0, stride, 0) == 0);
    EXPECT(rt::d3d12::copy_slots_between(nullptr, 8, 0, source.data(), 6, 0, stride, 1) == 0);
    return failures;
}

}  // namespace

int main() {
    test_slot_index_maps_inside_the_heap();
    test_slot_index_works_for_every_heap_type();
    test_slot_run_bytes_does_not_overflow();
    test_dirty_ranges_merge();
    test_no_slot_overlap_across_a_heap();
    test_copy_between_two_heaps();

    if (failures) {
        std::fprintf(stderr, "descriptor_handles_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("descriptor_handles_test: all tests passed\n");
    return 0;
}
