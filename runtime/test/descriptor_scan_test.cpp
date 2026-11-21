// Unit tests for the descriptor-table scan (the execution half of binding).
//
// A synthetic heap shadow is filled with encoded T#/V#/S# words and the scan must classify every slot, skip empty
// slots, clamp a bounded range and run an unbounded one to the end of the heap.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "descriptor_scan.h"
#include "descriptor_sizes.h"
#include "gfx10_descriptors.h"

namespace {

int failures = 0;

#define EXPECT(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

constexpr std::uint32_t kStride = rt::d3d12::kDescriptorSlotBytesCbvSrvUav;

void put(std::vector<std::uint8_t>& heap, std::uint32_t slot, const void* words, std::size_t bytes) {
    std::memcpy(heap.data() + std::size_t(slot) * kStride, words, bytes);
}

leonida::gfx10::ImageWords image_words() {
    leonida::gfx10::Image image{};
    image.address = 0x10000000;
    image.format = leonida::gfx10::format_from_vulkan(VK_FORMAT_R8G8B8A8_UNORM);
    image.width = 64;
    image.height = 64;
    image.mip_count = 1;
    return leonida::gfx10::encode(image);
}

leonida::gfx10::BufferWords buffer_words() {
    leonida::gfx10::Buffer buffer{};
    buffer.address = 0x20000000;
    buffer.stride = 16;
    buffer.num_records = 10;
    return leonida::gfx10::encode(buffer);
}

int test_scan_classifies_slots_and_skips_empty() {
    std::vector<std::uint8_t> heap(4 * kStride, 0);
    const auto image = image_words();
    const auto buffer = buffer_words();
    put(heap, 0, image.data(), sizeof(image));
    put(heap, 2, buffer.data(), sizeof(buffer));
    // slots 1 and 3 stay zero: never-written, must not be bound.

    const rt::d3d12::DescriptorHeapView view{heap.data(), 4, kStride, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    const auto scanned = rt::d3d12::scan_descriptor_table(view, 0, 4);
    EXPECT(scanned.size() == 2);
    EXPECT(scanned[0].slot == 0);
    EXPECT(scanned[0].kind == rt::d3d12::ScannedKind::image);
    EXPECT(scanned[1].slot == 2);
    EXPECT(scanned[1].kind == rt::d3d12::ScannedKind::buffer);
    // Different descriptors give different keys (the hash table needs that), and the image key is its T#.
    EXPECT(scanned[0].key != scanned[1].key);
    return failures;
}

int test_scan_boundaries() {
    std::vector<std::uint8_t> heap(4 * kStride, 0);
    const auto image = image_words();
    put(heap, 0, image.data(), sizeof(image));
    put(heap, 3, image.data(), sizeof(image));
    const rt::d3d12::DescriptorHeapView view{heap.data(), 4, kStride, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};

    EXPECT(rt::d3d12::scan_descriptor_table(view, 0, 2).size() == 1);              // bounded: slot 3 outside
    EXPECT(rt::d3d12::scan_descriptor_table(view, 0, 0xFFFFFFFFu).size() == 2);    // unbounded: to the end
    EXPECT(rt::d3d12::scan_descriptor_table(view, 3, 100).size() == 1);            // clamped to the end
    EXPECT(rt::d3d12::scan_descriptor_table(view, 4, 1).empty());                  // past the end
    EXPECT(rt::d3d12::scan_descriptor_table(view, 0, 0).empty());                  // empty request
    const rt::d3d12::DescriptorHeapView null_view{nullptr, 4, kStride, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    EXPECT(rt::d3d12::scan_descriptor_table(null_view, 0, 4).empty());
    return failures;
}

int test_scan_sampler_heap() {
    constexpr std::uint32_t sampler_stride = rt::d3d12::kDescriptorSlotBytesSampler;
    std::vector<std::uint8_t> heap(2 * sampler_stride, 0);
    leonida::gfx10::Sampler sampler{};
    const auto words = leonida::gfx10::encode(sampler);
    std::memcpy(heap.data(), words.data(), sizeof(words));

    const rt::d3d12::DescriptorHeapView view{heap.data(), 2, sampler_stride, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
    const auto scanned = rt::d3d12::scan_descriptor_table(view, 0, 0xFFFFFFFFu);
    // Both slots are samplers: a zero S# is a valid point/wrap sampler, not an unwritten slot.
    EXPECT(scanned.size() == 2);
    EXPECT(scanned[0].slot == 0 && scanned[0].kind == rt::d3d12::ScannedKind::sampler);
    EXPECT(scanned[1].slot == 1 && scanned[1].kind == rt::d3d12::ScannedKind::sampler);
    return failures;
}

int test_unknown_words_are_reported_not_bound() {
    std::vector<std::uint8_t> heap(kStride, 0xFF);  // every word set: decodes as neither a T# nor a V#
    const rt::d3d12::DescriptorHeapView view{heap.data(), 1, kStride, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    const auto scanned = rt::d3d12::scan_descriptor_table(view, 0, 1);
    EXPECT(scanned.size() == 1);
    EXPECT(scanned[0].kind == rt::d3d12::ScannedKind::unknown);
    return failures;
}

}  // namespace

int main() {
    test_scan_classifies_slots_and_skips_empty();
    test_scan_boundaries();
    test_scan_sampler_heap();
    test_unknown_words_are_reported_not_bound();

    if (failures) {
        std::fprintf(stderr, "descriptor_scan_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("descriptor_scan_test: all tests passed\n");
    return 0;
}
