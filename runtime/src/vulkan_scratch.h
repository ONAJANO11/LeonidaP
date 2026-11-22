// Scratch buffers that live until the submission that used them has completed: device-local chunks handed out
// while a list is prepared, retired with the submission's timeline value, and reused (or freed) once the GPU is past
// it. The replay stages copies Vulkan cannot do in one command through them (vulkan_replay.h, depth <-> colour
// copies). Single-threaded: the caller holds the context's lock. The same life cycle as the depth24 converter's own
// scratch (vulkan_depth24.h), which also needs descriptor pools and exists only on GPUs without D24.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace leonida {

class ScratchBuffers {
public:
    // A device-local allocation for a chunk; null on failure.
    using Allocate = std::function<VkDeviceMemory(const VkMemoryRequirements&)>;

    // What the chunks are for. The default is transfer scratch. `usage2` (VkBufferUsageFlags2, from VK_KHR_maintenance5)
    // replaces `usage` when non-zero, for bits the 32-bit field lacks (VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT);
    // `memory_type_mask` narrows the memory types a chunk may use, and `device_address` makes address() available (the
    // allocator must then allocate with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT).
    struct Config {
        VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        std::uint64_t usage2 = 0;
        std::uint32_t memory_type_mask = ~0u;
    };

    ScratchBuffers(PFN_vkGetDeviceProcAddr get, VkDevice device, Allocate allocate, Config config = {})
        : device_(device), allocate_(std::move(allocate)), config_(config) {
        loaded_ = vk_.load(get, device);
        vkGetBufferDeviceAddress_ = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(get(device, "vkGetBufferDeviceAddress"));
    }

    // The device address of `offset` in a buffer get() returned; 0 without vkGetBufferDeviceAddress.
    VkDeviceAddress address(VkBuffer buffer, VkDeviceSize offset) const {
        if (!vkGetBufferDeviceAddress_) return 0;
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = buffer;
        return vkGetBufferDeviceAddress_(device_, &info) + offset;
    }

    ~ScratchBuffers() {
        if (!loaded_) return;
        vk_.vkDeviceWaitIdle(device_);
        release(current_);
        for (auto& batch : retired_) release(batch);
        for (auto& chunk : spare_) destroy(chunk);
    }

    ScratchBuffers(const ScratchBuffers&) = delete;
    ScratchBuffers& operator=(const ScratchBuffers&) = delete;

    bool loaded() const { return loaded_; }

    // `bytes` of transfer scratch for the next submission, at a 256-byte aligned offset (more than any copy needs).
    bool get(std::uint64_t bytes, VkBuffer& buffer, VkDeviceSize& offset) {
        constexpr VkDeviceSize alignment = 256, minimum = 4u << 20;
        if (!loaded_ || !bytes) return false;
        for (auto& chunk : current_.chunks) {
            const VkDeviceSize start = (chunk.used + alignment - 1) / alignment * alignment;
            if (start <= chunk.size && bytes <= chunk.size - start) {
                chunk.used = start + bytes;
                buffer = chunk.buffer;
                offset = start;
                return true;
            }
        }
        Chunk chunk;
        for (std::size_t i = 0; i < spare_.size(); ++i)
            if (spare_[i].size >= bytes) {
                chunk = spare_[i];
                spare_.erase(spare_.begin() + std::ptrdiff_t(i));
                break;
            }
        if (!chunk.buffer && !make_chunk(std::max<std::uint64_t>(bytes, minimum), chunk)) return false;
        chunk.used = bytes;
        buffer = chunk.buffer;
        offset = 0;
        current_.chunks.push_back(chunk);
        return true;
    }

    // The chunks handed out since the last call belong to a submission that `timeline` reaches `ticket` for
    // (ticket 0: nothing was submitted, they are free once `collect` runs).
    void retire(VkSemaphore timeline, std::uint64_t ticket) {
        if (current_.chunks.empty()) return;
        current_.timeline = timeline;
        current_.ticket = ticket;
        retired_.push_back(std::move(current_));
        current_ = {};
    }

    // The submissions on `timeline` are over and the semaphore is about to go (its queue is idle).
    void forget(VkSemaphore timeline) {
        for (std::size_t i = 0; i < retired_.size();) {
            if (retired_[i].timeline != timeline) {
                ++i;
                continue;
            }
            release(retired_[i]);
            retired_.erase(retired_.begin() + std::ptrdiff_t(i));
        }
    }

    // Keeps for reuse (or frees) the chunks of finished submissions.
    void collect() {
        for (std::size_t i = 0; i < retired_.size();) {
            if (retired_[i].ticket != 0) {
                std::uint64_t done = 0;
                if (!retired_[i].timeline ||
                    vk_.vkGetSemaphoreCounterValue(device_, retired_[i].timeline, &done) != VK_SUCCESS ||
                    done < retired_[i].ticket) {
                    ++i;
                    continue;
                }
            }
            for (auto& chunk : retired_[i].chunks) {
                chunk.used = 0;
                if (spare_.size() < 4) spare_.push_back(chunk);
                else destroy(chunk);
            }
            retired_[i].chunks.clear();
            retired_.erase(retired_.begin() + std::ptrdiff_t(i));
        }
    }

private:
    struct Chunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0, used = 0;
    };
    struct Batch {
        std::vector<Chunk> chunks;
        VkSemaphore timeline = VK_NULL_HANDLE;
        std::uint64_t ticket = 0;
    };

    struct Functions {
#define LEONIDA_SCRATCH_FUNCTIONS(X)                                                                           \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkFreeMemory) \
    X(vkGetSemaphoreCounterValue) X(vkDeviceWaitIdle)
#define LEONIDA_SCRATCH_MEMBER(name) PFN_##name name = nullptr;
        LEONIDA_SCRATCH_FUNCTIONS(LEONIDA_SCRATCH_MEMBER)
#undef LEONIDA_SCRATCH_MEMBER

        bool load(PFN_vkGetDeviceProcAddr get, VkDevice device) {
            bool all = true;
#define LEONIDA_SCRATCH_LOAD(name)                              \
    name = reinterpret_cast<PFN_##name>(get(device, #name));    \
    all = all && name != nullptr;
            LEONIDA_SCRATCH_FUNCTIONS(LEONIDA_SCRATCH_LOAD)
#undef LEONIDA_SCRATCH_LOAD
#undef LEONIDA_SCRATCH_FUNCTIONS
            return all;
        }
    };

    bool make_chunk(std::uint64_t size, Chunk& out) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = config_.usage;
        VkBufferUsageFlags2CreateInfo usage2{VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO};
        usage2.usage = config_.usage2;
        if (config_.usage2) info.pNext = &usage2;
        if (vk_.vkCreateBuffer(device_, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vk_.vkGetBufferMemoryRequirements(device_, out.buffer, &requirements);
        requirements.memoryTypeBits &= config_.memory_type_mask;
        out.memory = allocate_ ? allocate_(requirements) : VK_NULL_HANDLE;
        if (!out.memory || vk_.vkBindBufferMemory(device_, out.buffer, out.memory, 0) != VK_SUCCESS) {
            destroy(out);
            return false;
        }
        out.size = size;
        return true;
    }
    void destroy(Chunk& chunk) {
        if (chunk.buffer) vk_.vkDestroyBuffer(device_, chunk.buffer, nullptr);
        if (chunk.memory) vk_.vkFreeMemory(device_, chunk.memory, nullptr);
        chunk = {};
    }
    void release(Batch& batch) {
        for (auto& chunk : batch.chunks) destroy(chunk);
        batch.chunks.clear();
    }

    VkDevice device_;
    Allocate allocate_;
    Config config_;
    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress_ = nullptr;
    Functions vk_;
    bool loaded_ = false;
    Batch current_;               // handed out, not yet submitted
    std::vector<Batch> retired_;  // submitted, waiting for the GPU
    std::vector<Chunk> spare_;    // finished chunks kept for reuse
};

}  // namespace leonida
