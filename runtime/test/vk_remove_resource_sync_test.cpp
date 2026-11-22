// VulkanContext::remove_resource() synchronization and view-lifetime regressions (runtime/src/vulkan_backend.h):
//   1. remove_resource() must hold resource_mutex_ and tracker_mutex_ together across resources_.erase(),
//      tracker_.forget(), and cached VkImageView retirement, preventing:
//      (a) premature defer_destroy() stamping before an in-flight VulkanQueue::execute() finishes submit(), and
//      (b) an ABA gap where a concurrent add_resource(id) on a reused ResourceId loses its LayoutTracker entry.
//   2. Concurrent add_resource / remove_resource + VulkanQueue::execute() preserves LayoutTracker consistency without
//      deadlocks or lost tracking.
//   3. Removing one image's cached views via views_.lower_bound() retires only that image's views and leaves all other
//      images' cached views intact.
// Skips with exit code 77 when no Vulkan 1.3 device is available. No game data.
#include "vk_test_device.h"
#include "../src/vulkan_backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

namespace driver {
PFN_vkGetDeviceProcAddr real_get_device = nullptr;
PFN_vkQueueSubmit real_submit = nullptr;
PFN_vkCreateImageView real_create_view = nullptr;
PFN_vkDestroyImageView real_destroy_view = nullptr;

std::function<VkResult()> on_next_submit;
std::atomic<std::uint32_t> created_views{0};
std::atomic<std::uint32_t> destroyed_views{0};

VKAPI_ATTR VkResult VKAPI_CALL queue_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits,
                                            VkFence fence) {
    if (on_next_submit) {
        auto cb = std::move(on_next_submit);
        on_next_submit = {};
        const VkResult override_res = cb();
        if (override_res != VK_SUCCESS) return override_res;
    }
    return real_submit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL create_image_view(VkDevice device, const VkImageViewCreateInfo* info,
                                                 const VkAllocationCallbacks* alloc, VkImageView* view) {
    const VkResult res = real_create_view(device, info, alloc, view);
    if (res == VK_SUCCESS) created_views.fetch_add(1, std::memory_order_relaxed);
    return res;
}

VKAPI_ATTR void VKAPI_CALL destroy_image_view(VkDevice device, VkImageView view, const VkAllocationCallbacks* alloc) {
    if (view != VK_NULL_HANDLE) destroyed_views.fetch_add(1, std::memory_order_relaxed);
    real_destroy_view(device, view, alloc);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device(VkDevice device, const char* name) {
    const std::string_view n = name;
    if (n == "vkQueueSubmit") return reinterpret_cast<PFN_vkVoidFunction>(queue_submit);
    if (n == "vkCreateImageView") return reinterpret_cast<PFN_vkVoidFunction>(create_image_view);
    if (n == "vkDestroyImageView") return reinterpret_cast<PFN_vkVoidFunction>(destroy_image_view);
    return real_get_device(device, name);
}

void install(PFN_vkGetDeviceProcAddr device_proc, VkDevice device) {
    real_get_device = device_proc;
    real_submit = reinterpret_cast<PFN_vkQueueSubmit>(device_proc(device, "vkQueueSubmit"));
    real_create_view = reinterpret_cast<PFN_vkCreateImageView>(device_proc(device, "vkCreateImageView"));
    real_destroy_view = reinterpret_cast<PFN_vkDestroyImageView>(device_proc(device, "vkDestroyImageView"));
}
}  // namespace driver

leonida::ResourceBinding make_color_rb(VkImage img, std::uint32_t width, std::uint32_t height,
                                       std::uint32_t layers = 1) {
    leonida::ResourceBinding rb{};
    rb.image = img;
    rb.format = VK_FORMAT_R8G8B8A8_UNORM;
    rb.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    rb.width = width;
    rb.height = height;
    rb.mip_levels = 1;
    rb.array_layers = layers;
    rb.depth = 1;
    rb.samples = 1;
    rb.color_attachment = true;
    return rb;
}

}  // namespace

int main() {
    using namespace leonida;

    vktest::Device d;
    if (!d.create(false)) {
        std::fprintf(stderr, "vk_remove_resource_sync_test: no Vulkan 1.3 device, skipped\n");
        return 77;
    }
    driver::install(d.device_proc, d.device);

    {
        VulkanContext context(driver::get_device, d.device, true);
        EXPECT(context.loaded());
        VulkanQueue queue(context, d.queue, d.family);
        VulkanQueue queue2(context, d.queue, d.family);
        EXPECT(queue.ok() && queue2.ok());

        const VkImage img1 = d.image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        const VkImage img2 = d.image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        const VkImage img3 = d.image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        EXPECT(img1 != VK_NULL_HANDLE && img2 != VK_NULL_HANDLE && img3 != VK_NULL_HANDLE);

        const ResourceBinding rb1 = make_color_rb(img1, 32, 32);
        const ResourceBinding rb2 = make_color_rb(img2, 32, 32);
        const ResourceBinding rb3 = make_color_rb(img3, 32, 32);
        context.add_resource(1, rb1, VK_IMAGE_LAYOUT_UNDEFINED);
        context.add_resource(3, rb3, VK_IMAGE_LAYOUT_UNDEFINED);

        // Rect clear forces VulkanContext::view() to create and cache a VkImageView in views_.
        const Rect rect{0, 0, 16, 16};
        CommandList rect_clear_1;
        EXPECT(rect_clear_1.clear_render_target({1, 0, 0, 1}, {0.2f, 0.4f, 0.6f, 1.0f}, {&rect, 1}));
        EXPECT(rect_clear_1.close());
        const CommandList* p1 = &rect_clear_1;

        CommandList dummy_list;
        EXPECT(dummy_list.clear_render_target({3, 0, 0, 1}, {0.1f, 0.1f, 0.1f, 1.0f}));
        EXPECT(dummy_list.close());
        const CommandList* p3 = &dummy_list;

        driver::created_views.store(0);
        driver::destroyed_views.store(0);
        EXPECT(queue.execute({&p1, 1}) == ReplayResult::success);
        EXPECT(queue.idle());
        EXPECT(driver::created_views.load() >= 1);

        // 1. Serialization across execute()'s resource_lock.unlock() -> submit() window:
        //    While execute() is inside vkQueueSubmit (resource_mutex_ unlocked, tracker_mutex_ + submit_mutex_ held),
        //    remove_resource(1) must acquire resource_mutex_ and block on tracker_mutex_ WITHOUT releasing
        //    resource_mutex_ first. That closes both the premature defer_destroy() ticket window and the ABA
        //    add_resource(1) vs tracker_.forget(1) gap.
        std::mutex sync_mu;
        std::condition_variable sync_cv;
        bool remover_started = false;
        bool remover_done = false;
        bool probe_started = false;
        bool probe_done = false;
        bool probe_passed_during_submit = false;
        std::thread remover;
        std::thread probe;

        driver::on_next_submit = [&]() -> VkResult {
            remover = std::thread([&] {
                {
                    std::lock_guard lk(sync_mu);
                    remover_started = true;
                }
                sync_cv.notify_all();
                context.remove_resource(1);
                {
                    std::lock_guard lk(sync_mu);
                    remover_done = true;
                }
                sync_cv.notify_all();
            });
            {
                std::unique_lock lk(sync_mu);
                sync_cv.wait_for(lk, std::chrono::seconds(2), [&] { return remover_started; });
                // Give remover time to enter remove_resource(1) and either block on tracker_mutex_ (fixed) or unlock
                // resource_mutex_ early (buggy).
                sync_cv.wait_for(lk, std::chrono::milliseconds(30), [&] { return remover_done; });
            }
            // Probe whether resource_mutex_ is still held by remover: query_heap_type() locks ONLY resource_mutex_
            // (shared), never tracker_mutex_ or submit_mutex_.
            probe = std::thread([&] {
                {
                    std::lock_guard lk(sync_mu);
                    probe_started = true;
                }
                sync_cv.notify_all();
                (void)context.query_heap_type(999999);
                {
                    std::lock_guard lk(sync_mu);
                    probe_done = true;
                }
                sync_cv.notify_all();
            });
            {
                std::unique_lock lk(sync_mu);
                sync_cv.wait_for(lk, std::chrono::seconds(2), [&] { return probe_started; });
                probe_passed_during_submit =
                    sync_cv.wait_for(lk, std::chrono::milliseconds(40), [&] { return probe_done; });
            }
            return VK_SUCCESS;
        };

        const ReplayResult exec_res = queue.execute({&p1, 1});
        if (remover.joinable()) remover.join();
        if (probe.joinable()) probe.join();
        EXPECT(exec_res == ReplayResult::success);
        EXPECT(remover_done);
        EXPECT(!probe_passed_during_submit);
        EXPECT(context.layout(1, 0) == VK_IMAGE_LAYOUT_UNDEFINED);

        // One subsequent submit advances completed_retire_ticket() past the view's safe_ticket (submitted_ticket_ + 1)
        // and drains the deferred VkImageView destruction cleanly.
        EXPECT(queue2.execute({&p3, 1}) == ReplayResult::success);
        EXPECT(queue.idle() && queue2.idle());
        context.drain_deleted_resources(context.completed_retire_ticket());
        EXPECT(driver::destroyed_views.load() >= 1);
        std::printf("remove_resource sync: serialized through tracker_mutex_ (no ABA or premature-retire gap) ok\n");

        // 2. Concurrent add_resource / remove_resource stress with concurrent VulkanQueue::execute().
        std::atomic<bool> stop_worker{false};
        std::atomic<bool> worker_failed{false};
        std::thread exec_worker([&] {
            while (!stop_worker.load(std::memory_order_relaxed)) {
                if (queue2.execute({&p3, 1}) != ReplayResult::success) {
                    worker_failed.store(true, std::memory_order_release);
                    break;
                }
            }
        });
        for (int i = 0; i < 80; ++i) {
            context.add_resource(2, rb2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            EXPECT(context.layout(2, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            context.remove_resource(2);
            EXPECT(context.layout(2, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
        }
        stop_worker.store(true, std::memory_order_relaxed);
        exec_worker.join();
        EXPECT(!worker_failed.load(std::memory_order_acquire));
        EXPECT(queue2.idle());
        std::printf("remove_resource sync: concurrent add/remove + execute stress ok\n");

        // 3. lower_bound() view retirement: among 16 images with 2 cached views each (2 array layers), removing one
        //    middle image retires only its 2 views while leaving the other 15 images' 30 views cached.
        constexpr int kImages = 16;
        std::vector<VkImage> imgs(kImages, VK_NULL_HANDLE);
        for (int i = 0; i < kImages; ++i) {
            imgs[i] = d.image(VK_FORMAT_R8G8B8A8_UNORM, 16, 16, 2,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            EXPECT(imgs[i] != VK_NULL_HANDLE);
            context.add_resource(ResourceId(100 + i), make_color_rb(imgs[i], 16, 16, 2), VK_IMAGE_LAYOUT_UNDEFINED);
        }

        const Rect small_rect{0, 0, 8, 8};
        CommandList populate_views;
        for (int i = 0; i < kImages; ++i) {
            EXPECT(populate_views.clear_render_target({ResourceId(100 + i), 0, 0, 1}, {0.1f, 0.2f, 0.3f, 1.0f},
                                                      {&small_rect, 1}));
            EXPECT(populate_views.clear_render_target({ResourceId(100 + i), 0, 1, 1}, {0.4f, 0.5f, 0.6f, 1.0f},
                                                      {&small_rect, 1}));
        }
        EXPECT(populate_views.close());
        const CommandList* pp = &populate_views;

        driver::created_views.store(0);
        driver::destroyed_views.store(0);
        EXPECT(queue.execute({&pp, 1}) == ReplayResult::success);
        EXPECT(queue.idle());
        EXPECT(driver::created_views.load() == kImages * 2);

        const int victim = kImages / 2;
        context.remove_resource(ResourceId(100 + victim));
        // Advance retire ticket by one submission + idle so victim's views (safe_ticket = submitted_ticket_ + 1) drain.
        EXPECT(queue2.execute({&p3, 1}) == ReplayResult::success);
        EXPECT(queue.idle() && queue2.idle());
        context.drain_deleted_resources(context.completed_retire_ticket());
        EXPECT(driver::destroyed_views.load() == 2);

        // Re-clearing all remaining 15 images must hit the existing cached views (0 new vkCreateImageView calls).
        CommandList check_remaining;
        for (int i = 0; i < kImages; ++i) {
            if (i == victim) continue;
            EXPECT(check_remaining.clear_render_target({ResourceId(100 + i), 0, 0, 1}, {0.2f, 0.3f, 0.4f, 1.0f},
                                                       {&small_rect, 1}));
            EXPECT(check_remaining.clear_render_target({ResourceId(100 + i), 0, 1, 1}, {0.5f, 0.6f, 0.7f, 1.0f},
                                                       {&small_rect, 1}));
        }
        EXPECT(check_remaining.close());
        const CommandList* pr = &check_remaining;

        driver::created_views.store(0);
        EXPECT(queue.execute({&pr, 1}) == ReplayResult::success);
        EXPECT(queue.idle());
        EXPECT(driver::created_views.load() == 0);

        for (int i = 0; i < kImages; ++i)
            if (i != victim) context.remove_resource(ResourceId(100 + i));
        context.remove_resource(3);
        EXPECT(queue.idle() && queue2.idle());
        std::printf("remove_resource sync: lower_bound targeted view retirement (2/32) ok\n");
    }

    EXPECT(d.validation_errors() == 0);
    std::printf("vk_remove_resource_sync_test: ok\n");
    return 0;
}
