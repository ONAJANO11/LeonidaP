// LayoutTracker::Transaction O(touched-subresources) rollback and steady-state zero-allocation tests
// (runtime/src/vulkan_replay.h). Pure CPU test — no GPU or Vulkan driver required. No game data.
#include "../src/vulkan_replay.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

static std::atomic<std::size_t> g_allocations{0};

#if defined(__GNUC__)
#define LEONIDA_TEST_NOINLINE __attribute__((noinline))
#else
#define LEONIDA_TEST_NOINLINE
#endif

LEONIDA_TEST_NOINLINE void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
LEONIDA_TEST_NOINLINE void* operator new[](std::size_t size) { return operator new(size); }
LEONIDA_TEST_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
LEONIDA_TEST_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
LEONIDA_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
LEONIDA_TEST_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using namespace leonida;

ResourceBinding make_color_binding(std::uint32_t mips = 1, std::uint32_t layers = 1) {
    ResourceBinding rb{};
    rb.format = VK_FORMAT_R8G8B8A8_UNORM;
    rb.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    rb.width = 64;
    rb.height = 64;
    rb.mip_levels = mips;
    rb.array_layers = layers;
    rb.depth = 1;
    rb.samples = 1;
    rb.color_attachment = true;
    return rb;
}

ResourceBinding make_ds_binding(std::uint32_t mips = 1, std::uint32_t layers = 1) {
    ResourceBinding rb{};
    rb.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    rb.aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    rb.width = 64;
    rb.height = 64;
    rb.mip_levels = mips;
    rb.array_layers = layers;
    rb.depth = 1;
    rb.samples = 1;
    return rb;
}

}  // namespace

int main() {
    // 1. Basic commit, explicit rollback, and RAII scope-exit rollback.
    {
        LayoutTracker tracker(true);
        const ResourceBinding rb = make_color_binding(1, 1);
        tracker.track(1, rb, VK_IMAGE_LAYOUT_UNDEFINED);
        tracker.track(2, rb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const VkImageSubresourceRange color0{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Explicit rollback restores multi-step transitions in reverse order.
        {
            LayoutTracker::Transaction tx(tracker);
            tracker.set(1, color0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            tracker.set(1, color0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            tracker.set(1, color0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            EXPECT(tracker.layout(1, 0) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            tx.rollback();
            tx.rollback();  // idempotent
        }
        EXPECT(tracker.layout(1, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
        EXPECT(tracker.layout(2, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Uncommitted RAII scope exit rolls back automatically.
        {
            LayoutTracker::Transaction tx(tracker);
            tracker.set(1, color0, VK_IMAGE_LAYOUT_GENERAL);
            tracker.set(2, color0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            EXPECT(tracker.layout(1, 0) == VK_IMAGE_LAYOUT_GENERAL);
            EXPECT(tracker.layout(2, 0) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        EXPECT(tracker.layout(1, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
        EXPECT(tracker.layout(2, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Commit retains changes; destructor after commit is a no-op.
        {
            LayoutTracker::Transaction tx(tracker);
            tracker.set(1, color0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            tracker.set(1, color0, VK_IMAGE_LAYOUT_GENERAL);
            tx.commit();
            tx.commit();    // idempotent
            tx.rollback();  // ignored after commit
        }
        EXPECT(tracker.layout(1, 0) == VK_IMAGE_LAYOUT_GENERAL);
    }

    // 2. Partial mip/layer ranges on a 4-mip x 3-layer texture.
    {
        LayoutTracker tracker(true);
        tracker.track(10, make_color_binding(4, 3), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const VkImageSubresourceRange partial{VK_IMAGE_ASPECT_COLOR_BIT, 1, 2, 1, 1};
        {
            LayoutTracker::Transaction tx(tracker);
            tracker.set(10, partial, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            tracker.set(10, {VK_IMAGE_ASPECT_COLOR_BIT, 2, 1, 1, 1}, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            EXPECT(tracker.layout(10, 1 + 1 * 4) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            EXPECT(tracker.layout(10, 2 + 1 * 4) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            EXPECT(tracker.layout(10, 0 + 1 * 4) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            tx.rollback();
        }
        for (std::uint32_t layer = 0; layer < 3; ++layer)
            for (std::uint32_t mip = 0; mip < 4; ++mip)
                EXPECT(tracker.layout(10, mip + layer * 4) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // 3. Separate vs merged depth/stencil planes.
    {
        LayoutTracker sep(true);
        const ResourceBinding ds = make_ds_binding(2, 1);
        sep.track(20, ds, VK_IMAGE_LAYOUT_UNDEFINED);
        sep.set(20, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 2, 0, 1}, VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL);
        {
            LayoutTracker::Transaction tx(sep);
            sep.set(20, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
            sep.set(20, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            EXPECT(sep.layout(20, 0) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            EXPECT(sep.layout(20, 2) == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL);
            tx.rollback();
        }
        EXPECT(sep.layout(20, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
        EXPECT(sep.layout(20, 1) == VK_IMAGE_LAYOUT_UNDEFINED);
        EXPECT(sep.layout(20, 2) == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL);
        EXPECT(sep.layout(20, 3) == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL);

        LayoutTracker merged(false);
        merged.track(21, ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        {
            LayoutTracker::Transaction tx(merged);
            merged.set(21, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            EXPECT(merged.layout(21, 0) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            EXPECT(merged.layout(21, 2) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            tx.rollback();
        }
        EXPECT(merged.layout(21, 0) == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        EXPECT(merged.layout(21, 2) == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    // 4. Nested transactions: inner rollback with outer commit, and inner commit with outer rollback.
    {
        LayoutTracker tracker(true);
        tracker.track(30, make_color_binding(1, 1), VK_IMAGE_LAYOUT_UNDEFINED);
        const VkImageSubresourceRange color0{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        {
            LayoutTracker::Transaction outer(tracker);
            tracker.set(30, color0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            {
                LayoutTracker::Transaction inner(tracker);
                tracker.set(30, color0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                EXPECT(tracker.layout(30, 0) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                inner.rollback();
            }
            EXPECT(tracker.layout(30, 0) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            outer.commit();
        }
        EXPECT(tracker.layout(30, 0) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        {
            LayoutTracker::Transaction outer(tracker);
            tracker.set(30, color0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            {
                LayoutTracker::Transaction inner(tracker);
                tracker.set(30, color0, VK_IMAGE_LAYOUT_GENERAL);
                inner.commit();
            }
            EXPECT(tracker.layout(30, 0) == VK_IMAGE_LAYOUT_GENERAL);
            outer.rollback();
        }
        EXPECT(tracker.layout(30, 0) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    // 5. Safe rollback even if a resource was forgotten, plus clean copy semantics.
    {
        LayoutTracker tracker(true);
        tracker.track(40, make_color_binding(1, 1), VK_IMAGE_LAYOUT_UNDEFINED);
        tracker.track(41, make_color_binding(1, 1), VK_IMAGE_LAYOUT_GENERAL);
        const VkImageSubresourceRange color0{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        {
            LayoutTracker::Transaction tx(tracker);
            tracker.set(40, color0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            tracker.set(41, color0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            tracker.forget(40);
            tx.rollback();
        }
        EXPECT(!tracker.tracked(40));
        EXPECT(tracker.layout(41, 0) == VK_IMAGE_LAYOUT_GENERAL);

        LayoutTracker copy = tracker;
        copy.set(41, color0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EXPECT(tracker.layout(41, 0) == VK_IMAGE_LAYOUT_GENERAL);
        EXPECT(copy.layout(41, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // 6. Steady-state zero heap allocations with 2,048 live tracked textures (both commit and rollback).
    {
        LayoutTracker tracker(true);
        const ResourceBinding rb = make_color_binding(1, 1);
        for (ResourceId id = 1000; id < 3048; ++id)
            tracker.track(id, rb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const VkImageSubresourceRange color0{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Warm up once so the reusable undo journal has capacity for 4 transitions.
        {
            LayoutTracker::Transaction tx(tracker);
            tracker.set(1000, color0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            tracker.set(1001, color0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            tracker.set(1000, color0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            tracker.set(1001, color0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            tx.rollback();
        }

        const std::size_t before = g_allocations.load(std::memory_order_relaxed);
        for (int round = 0; round < 200; ++round) {
            {
                LayoutTracker::Transaction tx(tracker);
                tracker.set(1000, color0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                tracker.set(1001, color0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                tracker.set(1000, color0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                tracker.set(1001, color0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                tx.commit();
            }
            {
                LayoutTracker::Transaction tx(tracker);
                tracker.set(1000, color0, VK_IMAGE_LAYOUT_GENERAL);
                tracker.set(1001, color0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                tracker.set(1000, color0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                tracker.set(1001, color0, VK_IMAGE_LAYOUT_GENERAL);
                tx.rollback();
            }
        }
        const std::size_t after = g_allocations.load(std::memory_order_relaxed);
        EXPECT(after == before);
        EXPECT(tracker.layout(1000, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EXPECT(tracker.layout(1001, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EXPECT(tracker.layout(3047, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Oversized spike (> 4,096 transitions) shrinks cleanly on commit without breaking later transactions.
        {
            LayoutTracker::Transaction tx(tracker);
            for (ResourceId id = 1000; id < 3048; ++id) {
                tracker.set(id, color0, VK_IMAGE_LAYOUT_GENERAL);
                tracker.set(id, color0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                tracker.set(id, color0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            tx.commit();
        }
        {
            LayoutTracker::Transaction tx(tracker);
            tracker.set(1000, color0, VK_IMAGE_LAYOUT_GENERAL);
            tx.rollback();
        }
        EXPECT(tracker.layout(1000, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    std::printf("layout_tracker_rollback_test: ok\n");
    return 0;
}
