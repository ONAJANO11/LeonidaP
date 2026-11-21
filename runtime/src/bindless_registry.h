#pragma once

// Bindless registry (P3/P5): the key -> (index, kind) map the device keeps per set-0 binding array.
//
// The allocator owns index bookkeeping; this class adds the kind of resource an index holds (so a release returns
// it to the right array) and the miss path the shader relies on: an unknown key looks up 0, the fallback slot.
// It is independent of Vulkan; the device pairs an index with its VkImageView/VkSampler and retires the pair through
// leonida::RetirementQueue.

#include <cstdint>
#include <map>

#include "bindless_allocator.h"
#include "descriptor_table.h"

namespace leonida {

enum class BindlessKind : std::uint8_t {
    image_2d = 0,
    image_2d_array,
    image_cube,
    image_1d,
    image_3d,
    image_storage,
    sampler,
};

struct BindlessEntry {
    std::uint32_t index = 0;  // 0 is the fallback, never a real entry
    BindlessKind kind = BindlessKind::image_2d;
};

class BindlessRegistry {
public:
    explicit BindlessRegistry(std::uint32_t capacity = 4096) : allocator_(capacity) {}

    // The index a key already owns (with its kind), or {0, kind} when the key is unknown.
    BindlessEntry find(const DescriptorKey& key, BindlessKind kind) const {
        const std::uint32_t index = allocator_.find(key);
        return index ? BindlessEntry{index, kind} : BindlessEntry{0, kind};
    }

    // The index for a key: the existing one (same kind) or a fresh allocation, 0 when the pool is full. A key that
    // was registered with a different kind is re-registered under the new kind, so the caller's later release frees
    // the slot it actually holds.
    BindlessEntry ensure(const DescriptorKey& key, BindlessKind kind) {
        const std::uint32_t existing = allocator_.find(key);
        if (existing) {
            auto it = kinds_.find(existing);
            if (it != kinds_.end() && it->second == kind) return {existing, kind};
            allocator_.release(existing);
            kinds_.erase(existing);
        }
        const std::uint32_t index = allocator_.ensure(key);
        if (!index) return {0, kind};
        kinds_[index] = kind;
        return {index, kind};
    }

    // Releases the slot for a key. False when the key is unknown or 0.
    bool release(const DescriptorKey& key) {
        const std::uint32_t index = allocator_.find(key);
        if (!index) return false;
        allocator_.release(index);
        kinds_.erase(index);
        return true;
    }

    bool release_index(std::uint32_t index) {
        if (!allocator_.in_use(index)) return false;
        kinds_.erase(index);
        // The allocator erases the key when it knows the index; release by index needs the key, so only the
        // allocator's own map knows it. Use release(key) when the key is available (the device always has it).
        return allocator_.release(index);
    }

    bool in_use(std::uint32_t index) const { return allocator_.in_use(index); }
    std::uint32_t allocated() const { return allocator_.allocated(); }
    std::uint32_t capacity() const { return allocator_.capacity(); }
    std::uint32_t lookup(const DescriptorKey& key) const { return allocator_.find(key); }

private:
    BindlessAllocator allocator_;
    std::map<std::uint32_t, BindlessKind> kinds_;
};

}  // namespace leonida
