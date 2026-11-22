// VulkanQueue::execute() O(touched-subresources) LayoutTracker rollback tests (runtime/src/vulkan_backend.h):
//   1. Multi-list ExecuteCommandLists that fails at vkQueueSubmit rolls back every touched subresource (keeping an
//      UNDEFINED committed texture UNDEFINED so its deferred zero-fill runs on retry) without disturbing untouched
//      tracked textures.
//   2. ExecuteCommandLists that fails at vkEndCommandBuffer rolls back multi-step layout transitions without calling
//      vkQueueSubmit.
//   3. Separate depth/stencil planes roll back independently on a failed submit.
//   4. Steady-state heap allocations in VulkanQueue::execute() are O(1) in the number of untouched tracked textures
//      (16 vs 2,048 tracked textures perform the same number of host allocations per execute).
// Skips with exit code 77 when no Vulkan 1.3 device is available. No game data.
#include "vk_test_device.h"
#include "../src/vulkan_backend.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
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

namespace driver {
PFN_vkGetDeviceProcAddr real_get_device = nullptr;
PFN_vkQueueSubmit real_submit = nullptr;
PFN_vkEndCommandBuffer real_end = nullptr;

std::atomic<VkResult> fail_next_submit{VK_SUCCESS};
std::atomic<VkResult> fail_next_end{VK_SUCCESS};
std::atomic<std::uint32_t> submit_calls{0};

VKAPI_ATTR VkResult VKAPI_CALL queue_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits,
                                            VkFence fence) {
    submit_calls.fetch_add(1, std::memory_order_relaxed);
    const VkResult armed = fail_next_submit.exchange(VK_SUCCESS);
    if (armed != VK_SUCCESS) return armed;
    return real_submit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL end_command_buffer(VkCommandBuffer cb) {
    const VkResult armed = fail_next_end.exchange(VK_SUCCESS);
    const VkResult real_rc = real_end(cb);
    if (armed != VK_SUCCESS) return armed;
    return real_rc;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device(VkDevice device, const char* name) {
    const std::string_view n = name;
    if (n == "vkQueueSubmit") return reinterpret_cast<PFN_vkVoidFunction>(queue_submit);
    if (n == "vkEndCommandBuffer") return reinterpret_cast<PFN_vkVoidFunction>(end_command_buffer);
    return real_get_device(device, name);
}

void install(PFN_vkGetDeviceProcAddr device_proc, VkDevice device) {
    real_get_device = device_proc;
    real_submit = reinterpret_cast<PFN_vkQueueSubmit>(device_proc(device, "vkQueueSubmit"));
    real_end = reinterpret_cast<PFN_vkEndCommandBuffer>(device_proc(device, "vkEndCommandBuffer"));
}
}  // namespace driver

// Fill every layer of a 2D RGBA8 image with 0x5A bytes on the GPU so a skipped zero-fill cannot pass on clean VRAM.
bool poison_image(vktest::Device& d, VkImage image, std::uint32_t width, std::uint32_t height, std::uint32_t layers) {
    const VkDeviceSize layer_bytes = VkDeviceSize(width) * height * 4;
    void* mapped = nullptr;
    const VkBuffer staging = d.buffer(layer_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &mapped);
    if (!staging || !mapped) return false;
    std::memset(mapped, 0x5A, std::size_t(layer_bytes));
    return d.run([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = image;
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                                               nullptr, 1, &to_dst);
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
            region.imageExtent = {width, height, 1};
            d.fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(
                cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        return true;
    });
}

// Transition a newly created image from UNDEFINED to GENERAL so registering it in GENERAL matches its Vulkan layout.
bool transition_image_to_general(vktest::Device& d, VkImage image) {
    return d.run([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_general.image = image;
        to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                                               nullptr, 1, &to_general);
        return true;
    });
}

}  // namespace

int main() {
    using namespace leonida;

    vktest::Device d;
    if (!d.create(false)) {
        std::fprintf(stderr, "vk_queue_tracker_rollback_test: no Vulkan 1.3 device, skipped\n");
        return 77;
    }
    driver::install(d.device_proc, d.device);

    {
        VulkanContext context(driver::get_device, d.device, true);
        EXPECT(context.loaded());
        VulkanQueue queue(context, d.queue, d.family);
        EXPECT(queue.ok());

        constexpr std::uint32_t width = 16, height = 16;
        const VkImage array_img = d.image(VK_FORMAT_R8G8B8A8_UNORM, width, height, 2,
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        const VkImage side_img = d.image(VK_FORMAT_R8G8B8A8_UNORM, width, height, 1,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        EXPECT(array_img != VK_NULL_HANDLE && side_img != VK_NULL_HANDLE);
        EXPECT(poison_image(d, array_img, width, height, 2));
        EXPECT(transition_image_to_general(d, side_img));

        constexpr std::uint32_t row_pitch = 256;  // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
        constexpr VkDeviceSize layer_bytes = VkDeviceSize(row_pitch) * height;
        void* readback_mapped = nullptr;
        const VkBuffer readback_buf = d.buffer(layer_bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback_mapped);
        EXPECT(readback_buf != VK_NULL_HANDLE && readback_mapped != nullptr);

        ResourceBinding array_rb{};
        array_rb.image = array_img;
        array_rb.format = VK_FORMAT_R8G8B8A8_UNORM;
        array_rb.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        array_rb.width = width;
        array_rb.height = height;
        array_rb.mip_levels = 1;
        array_rb.array_layers = 2;
        array_rb.depth = 1;
        array_rb.samples = 1;
        array_rb.color_attachment = true;
        context.add_resource(1, array_rb, VK_IMAGE_LAYOUT_UNDEFINED);

        ResourceBinding side_rb = array_rb;
        side_rb.image = side_img;
        side_rb.array_layers = 1;
        context.add_resource(2, side_rb, VK_IMAGE_LAYOUT_GENERAL);

        ResourceBinding rb_buf{};
        rb_buf.buffer = readback_buf;
        rb_buf.size = layer_bytes * 2;
        rb_buf.host_readable = true;
        context.add_resource(3, rb_buf);

        for (ResourceId extra = 1000; extra < 2024; ++extra)
            context.add_resource(extra, side_rb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // 1. Multi-list ExecuteCommandLists that fails at vkQueueSubmit rolls back both touched layers of resource 1.
        CommandList list0, list1;
        EXPECT(list0.transition({1, 0, d3d12_state::common, d3d12_state::render_target}));
        EXPECT(list0.clear_render_target({1, 0, 0, 1}, {0.25f, 0.50f, 0.75f, 1.0f}));
        EXPECT(list0.transition({1, 0, d3d12_state::render_target, d3d12_state::copy_source}));
        EXPECT(list0.close());

        EXPECT(list1.transition({1, 1, d3d12_state::common, d3d12_state::copy_source}));
        EXPECT(list1.close());

        const CommandList* batch[2]{&list0, &list1};
        driver::fail_next_submit.store(VK_ERROR_OUT_OF_HOST_MEMORY);
        EXPECT(queue.execute({batch, 2}) == ReplayResult::unsupported);
        EXPECT(context.layout(1, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
        EXPECT(context.layout(1, 1) == VK_IMAGE_LAYOUT_UNDEFINED);
        EXPECT(context.layout(1000, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EXPECT(context.layout(2023, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Retry succeeds: commits both layers, clears layer 0, and runs deferred zero-fill on layer 1.
        EXPECT(queue.execute({batch, 2}) == ReplayResult::success);
        EXPECT(context.layout(1, 0) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        EXPECT(context.layout(1, 1) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        CommandList copy_list;
        CopyTexture copy0{};
        copy0.dst = {3, true, 0, 0, width, height, 1, row_pitch};
        copy0.src = {1, false, 0, 0, 0, 0, 0, 0};
        CopyTexture copy1{};
        copy1.dst = {3, true, 0, layer_bytes, width, height, 1, row_pitch};
        copy1.src = {1, false, 1, 0, 0, 0, 0, 0};
        EXPECT(copy_list.copy_texture(copy0));
        EXPECT(copy_list.copy_texture(copy1));
        EXPECT(copy_list.close());
        const CommandList* copy_ptr = &copy_list;
        EXPECT(queue.execute({&copy_ptr, 1}) == ReplayResult::success);
        EXPECT(queue.idle());

        const auto* bytes = static_cast<const std::uint8_t*>(readback_mapped);
        EXPECT(std::abs(int(bytes[0]) - 64) <= 2);
        EXPECT(std::abs(int(bytes[1]) - 128) <= 2);
        EXPECT(std::abs(int(bytes[2]) - 191) <= 2);
        EXPECT(bytes[3] == 255);
        for (std::uint32_t row = 0; row < height; ++row) {
            const std::size_t base = std::size_t(layer_bytes) + std::size_t(row) * row_pitch;
            for (std::uint32_t col = 0; col < width * 4; ++col) EXPECT(bytes[base + col] == 0x00);
        }
        std::printf("queue tracker rollback: multi-list failed vkQueueSubmit rollback + deferred zero-fill ok\n");

        // 2. Failed vkEndCommandBuffer rolls back multi-step transitions without calling vkQueueSubmit.
        CommandList end_fail_list;
        EXPECT(end_fail_list.transition({2, 0, d3d12_state::common, d3d12_state::render_target}));
        EXPECT(end_fail_list.transition({2, 0, d3d12_state::render_target, d3d12_state::copy_source}));
        EXPECT(end_fail_list.close());
        const CommandList* end_ptr = &end_fail_list;

        driver::submit_calls.store(0);
        driver::fail_next_end.store(VK_ERROR_OUT_OF_HOST_MEMORY);
        EXPECT(queue.execute({&end_ptr, 1}) == ReplayResult::unsupported);
        EXPECT(driver::submit_calls.load() == 0);
        EXPECT(context.layout(2, 0) == VK_IMAGE_LAYOUT_GENERAL);

        EXPECT(queue.execute({&end_ptr, 1}) == ReplayResult::success);
        EXPECT(context.layout(2, 0) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        std::printf("queue tracker rollback: failed vkEndCommandBuffer rollback ok\n");

        // 3. Separate depth/stencil planes roll back independently on failed vkQueueSubmit.
        const VkFormat ds_format = d.depth_format_usable(VK_FORMAT_D32_SFLOAT_S8_UINT) ? VK_FORMAT_D32_SFLOAT_S8_UINT
                                 : d.depth_format_usable(VK_FORMAT_D24_UNORM_S8_UINT)  ? VK_FORMAT_D24_UNORM_S8_UINT
                                                                                       : VK_FORMAT_UNDEFINED;
        if (ds_format != VK_FORMAT_UNDEFINED) {
            const VkImage ds_img = d.image(ds_format, width, height, 1,
                                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            EXPECT(ds_img != VK_NULL_HANDLE);
            ResourceBinding ds_rb{};
            ds_rb.image = ds_img;
            ds_rb.format = ds_format;
            ds_rb.aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            ds_rb.width = width;
            ds_rb.height = height;
            ds_rb.mip_levels = 1;
            ds_rb.array_layers = 1;
            ds_rb.depth = 1;
            ds_rb.samples = 1;
            context.add_resource(4, ds_rb, VK_IMAGE_LAYOUT_UNDEFINED);

            // Commit stencil plane (subresource 1) to depth_read while leaving depth plane (subresource 0) UNDEFINED.
            CommandList init_stencil;
            EXPECT(init_stencil.transition({4, 1, d3d12_state::common, d3d12_state::depth_read}));
            EXPECT(init_stencil.close());
            const CommandList* sp = &init_stencil;
            EXPECT(queue.execute({&sp, 1}) == ReplayResult::success);
            const VkImageLayout stencil_committed = context.layout(4, 1);
            EXPECT(stencil_committed != VK_IMAGE_LAYOUT_UNDEFINED);
            EXPECT(context.layout(4, 0) == VK_IMAGE_LAYOUT_UNDEFINED);

            // Transition depth plane twice and fail submit: depth must return to UNDEFINED, stencil unchanged.
            CommandList depth_list;
            EXPECT(depth_list.transition({4, 0, d3d12_state::common, d3d12_state::depth_write}));
            EXPECT(depth_list.transition({4, 0, d3d12_state::depth_write, d3d12_state::copy_source}));
            EXPECT(depth_list.close());
            const CommandList* dp = &depth_list;
            driver::fail_next_submit.store(VK_ERROR_OUT_OF_HOST_MEMORY);
            EXPECT(queue.execute({&dp, 1}) == ReplayResult::unsupported);
            EXPECT(context.layout(4, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
            EXPECT(context.layout(4, 1) == stencil_committed);

            EXPECT(queue.execute({&dp, 1}) == ReplayResult::success);
            EXPECT(context.layout(4, 0) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            EXPECT(context.layout(4, 1) == stencil_committed);
            std::printf("queue tracker rollback: separate depth/stencil plane rollback ok\n");
        }

        // 4. O(1) allocation scaling: steady-state VulkanQueue::execute() host allocations must not grow with
        //    untouched tracked textures (16 vs 2,048 live tracked textures).
        for (ResourceId extra = 1016; extra < 2024; ++extra) context.remove_resource(extra);

        CommandList ping_pong;
        EXPECT(ping_pong.transition({2, 0, d3d12_state::copy_source, d3d12_state::render_target}));
        EXPECT(ping_pong.transition({2, 0, d3d12_state::render_target, d3d12_state::copy_source}));
        EXPECT(ping_pong.close());
        const CommandList* pp = &ping_pong;

        auto measure = [&](std::size_t& allocs_out, double& us_per_exec_out) -> bool {
            // Warm up command-slot reuse and undo-journal capacity.
            for (int w = 0; w < 3; ++w) {
                if (queue.execute({&pp, 1}) != ReplayResult::success) return false;
            }
            if (!queue.idle()) return false;
            constexpr int iters = 10;
            const std::size_t before = g_allocations.load(std::memory_order_relaxed);
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < iters; ++i) {
                if (queue.execute({&pp, 1}) != ReplayResult::success) return false;
            }
            const auto t1 = std::chrono::steady_clock::now();
            allocs_out = g_allocations.load(std::memory_order_relaxed) - before;
            us_per_exec_out = std::chrono::duration<double, std::micro>(t1 - t0).count() / double(iters);
            return queue.idle();
        };

        std::size_t allocs_16 = 0, allocs_2048 = 0;
        double us_16 = 0.0, us_2048 = 0.0;
        EXPECT(measure(allocs_16, us_16));

        for (ResourceId extra = 1016; extra < 3048; ++extra)
            context.add_resource(extra, side_rb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EXPECT(measure(allocs_2048, us_2048));

        std::printf("queue tracker scaling (10 executes): 16 textures = %zu allocs (%.1f us/exec), "
                    "2048 textures = %zu allocs (%.1f us/exec)\n",
                    allocs_16, us_16, allocs_2048, us_2048);
        // Cloning LayoutTracker per execute would add > 40,000 allocations over 10 calls; O(touched) adds 0.
        EXPECT(allocs_2048 <= allocs_16 + 64);

        for (ResourceId extra = 1000; extra < 3048; ++extra) context.remove_resource(extra);
        EXPECT(queue.idle());
    }

    EXPECT(d.validation_errors() == 0);
    std::printf("vk_queue_tracker_rollback_test: ok\n");
    return 0;
}
