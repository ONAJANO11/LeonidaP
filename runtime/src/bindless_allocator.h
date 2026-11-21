#pragma once

// Bindless index allocator (P3 of docs/d3d12_vulkan_analysis.md).
//
// One allocator per set-0 binding array (sampled 2D images, samplers, storage images, ...). Index 0 is the miss /
// fallback slot the shader's descriptor-hash-table lookup returns, so it is never handed out; ensure() maps a
// T#/S# key to a stable index, release() returns the slot, and a full pool returns 0.
//
// The allocator is deliberately independent of Vulkan and the device: the device owns the pools and the
// VkImageView/VkSampler arrays, this class owns only the index bookkeeping, so it can be tested on the CPU.

#include <cstdint>
#include <map>
#include <vector>

#include "descriptor_table.h"

namespace leonida {

class BindlessAllocator {
public:
    explicit BindlessAllocator(std::uint32_t capacity = 4096)
        : capacity_(capacity ? capacity : 1),
          used_(std::size_t(capacity_) + 1, false),
          keys_(std::size_t(capacity_)) {}

    std::uint32_t capacity() const { return capacity_; }
    std::uint32_t allocated() const { return allocated_; }

    // The index a key already owns, or 0 when the key is unknown (the shader's fallback).
    std::uint32_t find(const DescriptorKey& key) const {
        const auto it = by_key_.find(key);
        return it == by_key_.end() ? 0 : it->second;
    }

    // The index for a key: the existing one, a freshly allocated one, or 0 when the pool is full.
    std::uint32_t ensure(const DescriptorKey& key) {
        if (const std::uint32_t existing = find(key)) return existing;
        const std::uint32_t index = allocate();
        if (!index) return 0;
        keys_[index - 1] = key;
        by_key_.emplace(key, index);
        return index;
    }

    // A free index >= 1, or 0 when every slot is in use.
    std::uint32_t allocate() {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else if (next_ <= capacity_) {
            index = next_++;
        } else {
            return 0;
        }
        used_[index] = true;
        ++allocated_;
        return index;
    }

    // Frees a slot. 0 (the fallback), an out-of-range index, or a slot that is already free returns false.
    bool release(std::uint32_t index) {
        if (index == 0 || index > capacity_ || !used_[index]) return false;
        by_key_.erase(keys_[index - 1]);
        used_[index] = false;
        free_.push_back(index);
        --allocated_;
        return true;
    }

    bool in_use(std::uint32_t index) const {
        return index != 0 && index <= capacity_ && used_[index];
    }

    // Number of free slots, counting the never-used tail and the released list.
    std::uint32_t free_slots() const { return capacity_ - allocated_; }

private:
    std::uint32_t capacity_ = 0;
    std::uint32_t next_ = 1;  // first index never handed out; 0 is the fallback
    std::uint32_t allocated_ = 0;
    std::vector<bool> used_;   // [0] is the fallback and stays false
    std::vector<std::uint32_t> free_;
    std::vector<DescriptorKey> keys_;  // by index - 1, for release()
    std::map<DescriptorKey, std::uint32_t> by_key_;
};

}  // namespace leonida
