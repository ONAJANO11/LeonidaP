#pragma once

// Fence-value retirement queue (P5 of docs/d3d12_vulkan_analysis.md).
//
// A view or sampler can be referenced by command buffers that are still in flight, so it cannot be destroyed when
// the game releases it. The runtime retires it at the GPU timeline value that will have finished using it, and the
// queue releases it once the fence reports that value. Pure bookkeeping: the deleter is a closure, so the CPU tests
// can count releases without Vulkan.

#include <cstdint>
#include <functional>
#include <map>
#include <utility>

namespace leonida {

class RetirementQueue {
public:
    using Deleter = std::function<void()>;

    // Queues `deleter` for when the timeline reaches `fence_value`. A value of 0 means "already retired": it is
    // released by the next collect(0) or later, never destroyed on the spot.
    void retire(std::uint64_t fence_value, Deleter deleter) {
        if (!deleter) return;
        items_.emplace(fence_value, std::move(deleter));
        ++pending_;
    }

    // Releases every item whose fence value is <= completed_value. Returns how many were released.
    std::size_t collect(std::uint64_t completed_value) {
        std::size_t released = 0;
        auto it = items_.begin();
        while (it != items_.end() && it->first <= completed_value) {
            Deleter deleter = std::move(it->second);
            it = items_.erase(it);
            --pending_;
            deleter();  // outside the iteration's needs; a deleter must not touch this queue
            ++released;
        }
        return released;
    }

    std::size_t pending() const { return pending_; }

    // Items queued for a value, for tests and diagnostics.
    std::size_t pending_at(std::uint64_t fence_value) const { return items_.count(fence_value); }

private:
    std::multimap<std::uint64_t, Deleter> items_;
    std::size_t pending_ = 0;
};

}  // namespace leonida
