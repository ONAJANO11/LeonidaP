// ID3D12GraphicsCommandList1::OMSetDepthBounds on the Vulkan-backed COM surface, without a GPU: QueryInterface for
// GCL1, the method at the SDK's vtable slot, the values the list keeps for the next draw (clamped to [0, 1], NaN as
// 0, min > max kept), and the defaults after Reset and ClearState. d3d12_depth_bounds_test checks the pixels.
#include "../shims/d3d12_com.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
};

}  // namespace

int main() {
    using namespace rt::d3d12;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(create_command_allocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(create_command_list(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, IID_PPV_ARGS(&list.p))));
    Ref<ID3D12GraphicsCommandList1> list1;
    EXPECT(SUCCEEDED(list.p->QueryInterface(IID_PPV_ARGS(&list1.p))));
    EXPECT(static_cast<IUnknown*>(list1.p) == static_cast<IUnknown*>(list.p));

    FLOAT min = -1, max = -1;
    EXPECT(depth_bounds(list1.p, min, max));
    EXPECT(min == 0.0f && max == 1.0f);  // a new list starts at [0, 1]

    list1.p->OMSetDepthBounds(0.25f, 0.75f);
    EXPECT(depth_bounds(list1.p, min, max));
    EXPECT(min == 0.25f && max == 0.75f);

    // Outside [0, 1] is clamped (D3D12 requires the range; Vulkan does without VK_EXT_depth_range_unrestricted).
    list1.p->OMSetDepthBounds(-3.0f, 2.0f);
    EXPECT(depth_bounds(list1.p, min, max));
    EXPECT(min == 0.0f && max == 1.0f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    list1.p->OMSetDepthBounds(nan, std::numeric_limits<float>::infinity());
    EXPECT(depth_bounds(list1.p, min, max));
    EXPECT(min == 0.0f && max == 1.0f);

    // Min above max is kept: both APIs then reject every sample.
    list1.p->OMSetDepthBounds(0.75f, 0.25f);
    EXPECT(depth_bounds(list1.p, min, max));
    EXPECT(min == 0.75f && max == 0.25f);

    // The SDK's slot: IUnknown 0-2, ID3D12Object 3-6, ID3D12DeviceChild 7, ID3D12CommandList 8, GCL 9-59,
    // GCL1 60-65 (AtomicCopyBufferUINT, AtomicCopyBufferUINT64, OMSetDepthBounds, ...) in d3d12.h 10.0.26100.0.
    constexpr std::size_t kDepthBoundsSlot = 62;
    using DepthBoundsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList1*, FLOAT, FLOAT);
    void** vtable = *reinterpret_cast<void***>(list1.p);
    reinterpret_cast<DepthBoundsFn>(vtable[kDepthBoundsSlot])(list1.p, 0.125f, 0.5f);
    EXPECT(depth_bounds(list1.p, min, max));
    EXPECT(min == 0.125f && max == 0.5f);

    // ClearState and Reset return the bounds to [0, 1].
    list1.p->ClearState(nullptr);
    EXPECT(depth_bounds(list1.p, min, max));
    EXPECT(min == 0.0f && max == 1.0f);
    list1.p->OMSetDepthBounds(0.5f, 0.5f);
    EXPECT(list1.p->Close() == S_OK);
    EXPECT(SUCCEEDED(list1.p->Reset(allocator.p, nullptr)));
    EXPECT(depth_bounds(list1.p, min, max));
    EXPECT(min == 0.0f && max == 1.0f);
    EXPECT(list1.p->Close() == S_OK);

    EXPECT(!depth_bounds(nullptr, min, max));
    std::printf("GCL1: OMSetDepthBounds at slot 62, clamped to [0, 1], reset by ClearState and Reset\n");
    return 0;
}
