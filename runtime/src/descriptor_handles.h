#pragma once

// Descriptor-handle arithmetic shared by the Vulkan-backed device (P0/P1 of docs/d3d12_vulkan_analysis.md).
//
// CPU descriptor handles are pointers into a heap's host shadow, so the slot of a handle is
// (handle - base) / stride, and a heap owns [base, base + count * stride). Keeping this math in one header lets
// the device validate handles and mark dirty slots without duplicating it, and lets the CPU tests pin it without
// Vulkan, a GPU or the game.
//
// This header needs only <d3d12.h> (through descriptor_sizes.h), so it builds wherever the Windows SDK does.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "descriptor_sizes.h"

namespace rt::d3d12 {

// True when `handle` is slot-aligned inside [base, base + count * stride). On success `index` receives the slot
// number. A stride or count of 0 never matches, so an empty heap cannot own a handle.
inline bool slot_index(std::uintptr_t handle, std::uintptr_t base, std::uint32_t count, std::uint32_t stride,
                       std::uint32_t* index = nullptr) {
    if (!stride || !count || handle < base) return false;
    const std::uint64_t offset = static_cast<std::uint64_t>(handle) - base;
    if (offset % stride != 0) return false;
    const std::uint64_t slot = offset / stride;
    if (slot >= count) return false;
    if (index) *index = static_cast<std::uint32_t>(slot);
    return true;
}

// Bytes a run of `count` slots occupies. False when the multiplication would overflow size_t, so a corrupt range
// can never turn into a short memcpy that silently copies less than the caller asked for.
inline bool slot_run_bytes(std::uint32_t count, std::uint32_t stride, std::size_t* bytes) {
    if (!bytes) return false;
    const std::uint64_t total = static_cast<std::uint64_t>(count) * stride;
    if (total > std::numeric_limits<std::size_t>::max()) return false;
    *bytes = static_cast<std::size_t>(total);
    return true;
}

// A half-open slot range [first, end). Empty when end <= first; count() is what the device marks dirty.
struct DirtyRange {
    std::uint32_t first = 0;
    std::uint32_t end = 0;

    bool empty() const { return end <= first; }
    std::uint32_t count() const { return empty() ? 0 : end - first; }
};

// The smallest range covering both. An empty range is the identity, so a heap that has never been written stays
// empty and the bindless scan can skip it entirely.
inline DirtyRange merge(DirtyRange a, DirtyRange b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.first, b.first), std::max(a.end, b.end)};
}

inline DirtyRange slot_range(std::uint32_t first, std::uint32_t count) {
    return {first, static_cast<std::uint32_t>(first + count)};
}

// Copies up to `count` slots between two heaps' host shadows, clamped to both heaps. Returns how many slots were
// copied, so the caller can mark exactly that range dirty. Two heaps in one call is the P1 test: D3D12 lets a copy
// run from one heap to another, and the destination stride is the destination heap's.
inline std::uint32_t copy_slots_between(std::uint8_t* destination, std::uint32_t destination_slots,
                                        std::uint32_t destination_first, const std::uint8_t* source,
                                        std::uint32_t source_slots, std::uint32_t source_first, std::uint32_t stride,
                                        std::uint32_t count) {
    if (!destination || !source || !stride || !count) return 0;
    if (destination_first >= destination_slots || source_first >= source_slots) return 0;
    const std::uint32_t copied = std::min({count, destination_slots - destination_first, source_slots - source_first});
    if (!copied) return 0;
    std::memmove(destination + std::size_t(destination_first) * stride, source + std::size_t(source_first) * stride,
                 std::size_t(copied) * stride);
    return copied;
}

}  // namespace rt::d3d12
