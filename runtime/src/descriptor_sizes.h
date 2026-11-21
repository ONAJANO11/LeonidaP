#pragma once

// Per-type D3D12 descriptor slot sizes (docs/d3d12_vulkan.md section 3.4; P0 of docs/d3d12_vulkan_analysis.md).
//
// P0 replaces the single `kDescriptorSize = 32` in shims/d3d12_vulkan_device.cpp with one table, so the sampler
// heap and the RTV/DSV heaps stop sharing the CBV/SRV/UAV stride and `GetDescriptorHandleIncrementSize` answers
// per heap type. The console increments are still open (docs/d3d12_vulkan.md:354-358); the values below are the
// documented starting point, and a measured value replaces one line here without touching any call site.
//
//   CBV/SRV/UAV: one GFX10 T# = 8 dwords = 32 bytes.
//   Sampler:     one S# = 4 dwords, padded to 32 bytes.
//   RTV/DSV:     the runtime's own TargetView payload, padded to 32 bytes.
//
// This header needs only <d3d12.h> (the Windows SDK), so its contract can be tested on the CPU without Vulkan,
// a GPU or the game.

#include <d3d12.h>

namespace rt::d3d12 {

inline constexpr UINT kDescriptorSlotBytesCbvSrvUav = 32;
inline constexpr UINT kDescriptorSlotBytesSampler = 32;
inline constexpr UINT kDescriptorSlotBytesRtv = 32;
inline constexpr UINT kDescriptorSlotBytesDsv = 32;

// The slot size for a heap type, or 0 for an invalid/unknown type. Callers (CreateDescriptorHeap,
// GetDescriptorHandleIncrementSize) reject 0 so a heap can never be created with a stride of zero.
constexpr UINT descriptor_slot_bytes(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    switch (type) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return kDescriptorSlotBytesCbvSrvUav;
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:     return kDescriptorSlotBytesSampler;
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:         return kDescriptorSlotBytesRtv;
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:         return kDescriptorSlotBytesDsv;
        default:                                     return 0;
    }
}

}  // namespace rt::d3d12
