#pragma once

// Descriptor-table scan (the "second half" of binding): turn the slots of a bound heap range into the bindless
// requests the replay must satisfy before a draw.
//
// The scan reads the heap's host shadow exactly as a shader would read the descriptor: a T# (image), a V# (buffer or
// CBV) or an S# (sampler). Empty slots are skipped, an unbounded range (UINT32_MAX descriptors) runs to the end of
// the heap, and a slot whose words decode as neither is reported as unknown rather than silently bound.
//
// No Vulkan, no device, no game data: gfx10_descriptors.h supplies the encoders/decoders, so the CPU tests build a
// synthetic heap and check what the scan returns.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <d3d12.h>

#include "descriptor_table.h"
#include "gfx10_descriptors.h"

namespace rt::d3d12 {

enum class ScannedKind : std::uint8_t { empty, buffer, image, sampler, unknown };

struct ScannedDescriptor {
    std::uint32_t slot = 0;               // slot index inside the heap
    leonida::DescriptorKey key = {};      // T# key (image), V# first four words zero-padded (buffer) or S# key
    ScannedKind kind = ScannedKind::empty;
};

struct DescriptorHeapView {
    const std::uint8_t* base = nullptr;  // the heap's host shadow
    std::uint32_t slots = 0;
    std::uint32_t stride = 0;            // descriptor_slot_bytes(type)
    D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
};

namespace detail {

inline bool all_zero(const std::uint8_t* data, std::uint32_t bytes) {
    for (std::uint32_t i = 0; i < bytes; ++i)
        if (data[i]) return false;
    return true;
}

}  // namespace detail

// Scans slots [first, first + count) of the heap. count == UINT32_MAX means "to the end of the heap". A range that
// starts at or past the end yields nothing; a range that runs past the end is clamped.
inline std::vector<ScannedDescriptor> scan_descriptor_table(const DescriptorHeapView& heap, std::uint32_t first,
                                                            std::uint32_t count) {
    std::vector<ScannedDescriptor> out;
    if (!heap.base || !heap.stride || first >= heap.slots) return out;
    const std::uint32_t available = heap.slots - first;
    const std::uint32_t slots = count == 0xFFFFFFFFu ? available : std::min(count, available);
    out.reserve(slots);

    for (std::uint32_t i = 0; i < slots; ++i) {
        const std::uint8_t* bytes = heap.base + std::size_t(first + i) * heap.stride;
        // A zero T#/V# is a slot the game never wrote. A zero S# is not: an all-zero sampler descriptor is a valid
        // (point, wrap, no mip) sampler, so sampler heaps are never skipped.
        if (heap.type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
            detail::all_zero(bytes, std::min<std::uint32_t>(heap.stride, 32)))
            continue;

        ScannedDescriptor scanned{};
        scanned.slot = first + i;
        if (heap.type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
            if (heap.stride < 16) {
                scanned.kind = ScannedKind::unknown;
                out.push_back(scanned);
                continue;
            }
            leonida::gfx10::SamplerWords words{};
            std::memcpy(words.data(), bytes, sizeof(words));
            scanned.key = leonida::make_ssharp_key(words.data());
            scanned.kind = ScannedKind::sampler;
            out.push_back(scanned);
            continue;
        }

        if (heap.type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || heap.stride < 32) {
            scanned.kind = ScannedKind::unknown;
            out.push_back(scanned);
            continue;
        }
        leonida::gfx10::ImageWords image_words{};
        std::memcpy(image_words.data(), bytes, sizeof(image_words));
        if (leonida::gfx10::is_image(image_words)) {
            leonida::gfx10::Image image{};
            if (leonida::gfx10::decode(image_words, image)) {
                scanned.key = leonida::make_tsharp_key(image_words.data());
                scanned.kind = ScannedKind::image;
            } else {
                scanned.kind = ScannedKind::unknown;
            }
        } else {
            leonida::gfx10::BufferWords buffer_words{};
            std::memcpy(buffer_words.data(), bytes, sizeof(buffer_words));
            leonida::gfx10::Buffer buffer{};
            if (leonida::gfx10::decode(buffer_words, buffer)) {
                scanned.key = {};  // a V# is dereferenced by its VA; the key only needs to be stable per slot
                std::memcpy(scanned.key.data(), buffer_words.data(), sizeof(buffer_words));
                scanned.kind = ScannedKind::buffer;
            } else {
                scanned.kind = ScannedKind::unknown;
            }
        }
        out.push_back(scanned);
    }
    return out;
}

}  // namespace rt::d3d12
