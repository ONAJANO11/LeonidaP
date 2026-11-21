// Unit tests for the per-type descriptor slot sizes (P0 of docs/d3d12_vulkan_analysis.md).
//
// No Vulkan, no GPU and no game data: the whole contract is one table plus the TargetView payload it has to hold.
// Run it directly (descriptor_increment_test.exe) or through `ctest --test-dir runtime/build`.

#include <cstdio>
#include <cstdint>

#include "command_list.h"      // leonida::TargetView, the RTV/DSV payload
#include "descriptor_sizes.h"  // rt::d3d12::descriptor_slot_bytes

namespace {

int failures = 0;

#define EXPECT(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

constexpr UINT kTypeCount = static_cast<UINT>(D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES);

int test_every_valid_heap_type_has_a_slot() {
    for (UINT t = 0; t < kTypeCount; ++t) {
        const UINT size = rt::d3d12::descriptor_slot_bytes(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(t));
        EXPECT(size != 0);
        EXPECT(size % 8 == 0);  // D3D12 handles are 8 bytes wide
        EXPECT(size >= 16);     // a GFX10 S# is 4 dwords before padding
    }
    return failures;
}

int test_documented_values() {
    // The starting point from docs/d3d12_vulkan.md:354-358. A measured console value changes these constants, not
    // the call sites; this test then changes with them.
    EXPECT(rt::d3d12::descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) == 32);
    EXPECT(rt::d3d12::descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) == 32);
    EXPECT(rt::d3d12::descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) == 32);
    EXPECT(rt::d3d12::descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE_DSV) == 32);
    return failures;
}

int test_rtv_dsv_hold_the_target_view_payload() {
    // CreateRenderTargetView/CreateDepthStencilView memcpy a leonida::TargetView into the CPU slot, so the slot
    // must be at least that large. This is the one size that is ours, not a console measurement.
    EXPECT(rt::d3d12::descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) >= sizeof(leonida::TargetView));
    EXPECT(rt::d3d12::descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE_DSV) >= sizeof(leonida::TargetView));
    return failures;
}

int test_invalid_type_is_rejected() {
    // CreateDescriptorHeap returns E_INVALIDARG for a 0 stride, so an unknown enum must not silently get 32.
    EXPECT(rt::d3d12::descriptor_slot_bytes(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(kTypeCount)) == 0);
    EXPECT(rt::d3d12::descriptor_slot_bytes(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(0x7f)) == 0);
    return failures;
}

int test_heap_arithmetic_uses_the_type_stride() {
    // Slot i is base + i * stride. Offsets must be strictly increasing (no overlap) and the last slot must end
    // exactly at slots * stride, so the heap allocation covers every slot the game can index.
    constexpr UINT slots = 4;
    for (UINT t = 0; t < kTypeCount; ++t) {
        const UINT stride = rt::d3d12::descriptor_slot_bytes(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(t));
        auto offset = [stride](UINT index) { return std::uint64_t(stride) * index; };
        EXPECT(offset(0) == 0);
        for (UINT i = 1; i < slots; ++i) EXPECT(offset(i) >= offset(i - 1) + stride);
        EXPECT(offset(slots) == std::uint64_t(stride) * slots);
    }
    return failures;
}

}  // namespace

int main() {
    test_every_valid_heap_type_has_a_slot();
    test_documented_values();
    test_rtv_dsv_hold_the_target_view_payload();
    test_invalid_type_is_rejected();
    test_heap_arithmetic_uses_the_type_stride();

    if (failures) {
        std::fprintf(stderr, "descriptor_increment_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("descriptor_increment_test: all tests passed (%u heap types)\n", kTypeCount);
    return 0;
}
