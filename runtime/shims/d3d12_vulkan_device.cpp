// A Vulkan-backed ID3D12Device (PC ABI) for the Xbox-ABI device proxy (d3d12_xs_device.cpp) to forward to.
// Implemented: queues, allocators, command lists, fences, descriptor heaps, render-target and depth-stencil views,
// committed buffers and textures (2D, multisampled 2D, 3D), a few feature queries. Everything else is a generated stub that reports
// itself once. See docs/d3d12_vulkan.md section 4.1/4.2 for what each answer means and what is still open.
#include "d3d12_com.h"

#include "d3d12_stubs.h"  // generated
#include "d3d12_com_object.h"
#include "d3d12_xs_device.h"
#include "../src/shader_library.h"
#include "../src/vulkan_present.h"

#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <map>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rt::d3d12 {
namespace {

// Every descriptor slot is this many bytes; an RTV or DSV slot holds a leonida::TargetView. The console's own
// increments are open question 4 in docs/d3d12_vulkan.md; the application reads them from the device, as it must.
constexpr UINT kDescriptorSize = descriptor_size;
static_assert(sizeof(leonida::TargetView) <= kDescriptorSize);

void log_once(const char* what, int value) {
    static std::mutex mutex;
    static std::set<std::pair<std::string, int>> seen;
    std::lock_guard lock(mutex);
    if (seen.insert({what, value}).second) std::fprintf(stderr, "[rt] d3d12: %s %d\n", what, value);
}

struct Format {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspects = 0;
    bool typeless = false;  // created with MUTABLE_FORMAT so typed views can reinterpret it
};

// DXGI -> Vulkan for the formats render targets, depth buffers and common textures use. Typeless depth families
// become depth formats when the resource allows depth-stencil.
Format vulkan_format(DXGI_FORMAT format, bool depth) {
    constexpr VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT, d = VK_IMAGE_ASPECT_DEPTH_BIT,
                                 ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return {VK_FORMAT_R32G32B32A32_SFLOAT, color, true};
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return {VK_FORMAT_R32G32B32A32_SFLOAT, color};
    case DXGI_FORMAT_R32G32B32A32_UINT: return {VK_FORMAT_R32G32B32A32_UINT, color};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return {VK_FORMAT_R16G16B16A16_SFLOAT, color, true};
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return {VK_FORMAT_R16G16B16A16_SFLOAT, color};
    case DXGI_FORMAT_R16G16B16A16_UNORM: return {VK_FORMAT_R16G16B16A16_UNORM, color};
    case DXGI_FORMAT_R32G32_FLOAT: return {VK_FORMAT_R32G32_SFLOAT, color};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, color, true};
    case DXGI_FORMAT_R10G10B10A2_UNORM: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, color};
    case DXGI_FORMAT_R11G11B10_FLOAT: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, color};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return {VK_FORMAT_R8G8B8A8_UNORM, color, true};
    case DXGI_FORMAT_R8G8B8A8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, color};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, color};
    case DXGI_FORMAT_R8G8B8A8_UINT: return {VK_FORMAT_R8G8B8A8_UINT, color};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return {VK_FORMAT_B8G8R8A8_UNORM, color, true};
    case DXGI_FORMAT_B8G8R8A8_UNORM: return {VK_FORMAT_B8G8R8A8_UNORM, color};
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return {VK_FORMAT_B8G8R8A8_SRGB, color};
    case DXGI_FORMAT_R16G16_FLOAT: return {VK_FORMAT_R16G16_SFLOAT, color};
    case DXGI_FORMAT_R8G8_UNORM: return {VK_FORMAT_R8G8_UNORM, color};
    case DXGI_FORMAT_R16_FLOAT: return {VK_FORMAT_R16_SFLOAT, color};
    case DXGI_FORMAT_R8_UNORM: return {VK_FORMAT_R8_UNORM, color};
    case DXGI_FORMAT_R32_FLOAT: return {VK_FORMAT_R32_SFLOAT, color};
    case DXGI_FORMAT_R32_UINT: return {VK_FORMAT_R32_UINT, color};
    case DXGI_FORMAT_BC1_TYPELESS: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC1_UNORM: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC1_UNORM_SRGB: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, color};
    case DXGI_FORMAT_BC2_TYPELESS: return {VK_FORMAT_BC2_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC2_UNORM: return {VK_FORMAT_BC2_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC2_UNORM_SRGB: return {VK_FORMAT_BC2_SRGB_BLOCK, color};
    case DXGI_FORMAT_BC3_TYPELESS: return {VK_FORMAT_BC3_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC3_UNORM: return {VK_FORMAT_BC3_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC3_UNORM_SRGB: return {VK_FORMAT_BC3_SRGB_BLOCK, color};
    case DXGI_FORMAT_BC4_TYPELESS: return {VK_FORMAT_BC4_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC4_UNORM: return {VK_FORMAT_BC4_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC4_SNORM: return {VK_FORMAT_BC4_SNORM_BLOCK, color};
    case DXGI_FORMAT_BC5_TYPELESS: return {VK_FORMAT_BC5_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC5_UNORM: return {VK_FORMAT_BC5_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC5_SNORM: return {VK_FORMAT_BC5_SNORM_BLOCK, color};
    case DXGI_FORMAT_BC6H_TYPELESS: return {VK_FORMAT_BC6H_UFLOAT_BLOCK, color, true};
    case DXGI_FORMAT_BC6H_UF16: return {VK_FORMAT_BC6H_UFLOAT_BLOCK, color};
    case DXGI_FORMAT_BC6H_SF16: return {VK_FORMAT_BC6H_SFLOAT_BLOCK, color};
    case DXGI_FORMAT_BC7_TYPELESS: return {VK_FORMAT_BC7_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC7_UNORM: return {VK_FORMAT_BC7_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC7_UNORM_SRGB: return {VK_FORMAT_BC7_SRGB_BLOCK, color};
    case DXGI_FORMAT_D32_FLOAT: return {VK_FORMAT_D32_SFLOAT, d};
    case DXGI_FORMAT_D16_UNORM: return {VK_FORMAT_D16_UNORM, d};
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return {VK_FORMAT_D32_SFLOAT_S8_UINT, ds};
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return {VK_FORMAT_D24_UNORM_S8_UINT, ds};
    case DXGI_FORMAT_R32_TYPELESS:
        return depth ? Format{VK_FORMAT_D32_SFLOAT, d, true} : Format{VK_FORMAT_R32_SFLOAT, color, true};
    case DXGI_FORMAT_R16_TYPELESS:
        return depth ? Format{VK_FORMAT_D16_UNORM, d, true} : Format{VK_FORMAT_R16_UNORM, color, true};
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return depth ? Format{VK_FORMAT_D32_SFLOAT_S8_UINT, ds, true} : Format{};
    case DXGI_FORMAT_R24G8_TYPELESS:
        return depth ? Format{VK_FORMAT_D24_UNORM_S8_UINT, ds, true} : Format{};
    default: return {};
    }
}

// What a device's children need from it, kept apart from the COM class so children can hold a plain pointer.
struct DeviceState {
    VulkanDeviceInfo info;
    std::unique_ptr<leonida::VulkanContext> context;
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkGetPhysicalDeviceFormatProperties format_properties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties image_format_properties = nullptr;

    // The GPU address arena (docs/d3d12_vulkan.md section 3.3): one host-visible, device-addressable VkBuffer that
    // stands in for the console's GPU address space. A console VA is an offset in it, and heap_base (push-constant
    // dwords 32-33) is its device address. It is at most 4 GiB, so every VA fits the 32-bit descriptor-table
    // pointers some shaders use. The first 64 KiB are never handed out, so no resource has VA 0.
    struct Arena {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint8_t* mapped = nullptr;
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0;
        std::mutex mutex;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size

        // The high-water mark (D3D-55, open question 5): the arena's size is a guess until a run measures it.
        // `peak_end` is the highest end offset ever handed out, the size the arena must have; `peak_in_use` is the
        // most bytes live at once, which differs from it by fragmentation.
        bool log = false;  // LEONIDA_ARENA_LOG=1: one line per VA handed out
        VkDeviceSize in_use = 0, peak_in_use = 0, peak_end = 0;
        VkDeviceSize use_in_use[3] = {}, use_peak[3] = {};
        std::uint64_t allocations = 0, failures = 0;
    } arena;

    enum ArenaUse { arena_buffer, arena_descriptor_heap, arena_shader };
    static constexpr const char* kArenaUseNames[3] = {"buffer", "descriptor heap", "shader"};

    bool arena_allocate(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize& out, ArenaUse use) {
        std::lock_guard lock(arena.mutex);
        for (auto it = arena.free.begin(); it != arena.free.end(); ++it) {
            const VkDeviceSize start = (it->first + alignment - 1) / alignment * alignment;
            const VkDeviceSize end = it->first + it->second;
            if (start + bytes > end) continue;
            const VkDeviceSize block = it->first;
            arena.free.erase(it);
            if (start > block) arena.free[block] = start - block;
            if (start + bytes < end) arena.free[start + bytes] = end - (start + bytes);
            out = start;
            ++arena.allocations;
            arena.in_use += bytes;
            arena.peak_in_use = std::max(arena.peak_in_use, arena.in_use);
            arena.peak_end = std::max(arena.peak_end, start + bytes);
            arena.use_in_use[use] += bytes;
            arena.use_peak[use] = std::max(arena.use_peak[use], arena.use_in_use[use]);
            if (arena.log)
                std::fprintf(stderr, "[rt] d3d12: arena %s VA 0x%llx + 0x%llx\n", kArenaUseNames[use],
                             (unsigned long long)start, (unsigned long long)bytes);
            return true;
        }
        ++arena.failures;
        if (arena.log)
            std::fprintf(stderr, "[rt] d3d12: arena %s: no room for 0x%llx bytes\n", kArenaUseNames[use],
                         (unsigned long long)bytes);
        return false;
    }

    // A console VA outside the arena (an upload or readback buffer: its device address minus heap_base).
    void log_va(const char* what, std::uint64_t va, VkDeviceSize bytes) {
        if (arena.log)
            std::fprintf(stderr, "[rt] d3d12: %s VA 0x%llx + 0x%llx (outside the arena)\n", what,
                         (unsigned long long)va, (unsigned long long)bytes);
    }

    // Printed once at device release, so a run reports the size the arena needs.
    void report_arena() {
        std::lock_guard lock(arena.mutex);
        if (!arena.size) return;
        const double mib = 1.0 / (1 << 20);
        std::fprintf(stderr,
                     "[rt] d3d12: GPU address arena: peak end %.1f of %.1f MiB (%.1f%%), peak live %.1f MiB "
                     "(buffers %.1f, descriptor heaps %.1f, shaders %.1f), %llu allocations, %llu refused\n",
                     arena.peak_end * mib, arena.size * mib, 100.0 * double(arena.peak_end) / double(arena.size),
                     arena.peak_in_use * mib, arena.use_peak[arena_buffer] * mib,
                     arena.use_peak[arena_descriptor_heap] * mib, arena.use_peak[arena_shader] * mib,
                     (unsigned long long)arena.allocations, (unsigned long long)arena.failures);
    }

    void arena_release(VkDeviceSize offset, VkDeviceSize bytes, ArenaUse use) {
        std::lock_guard lock(arena.mutex);
        arena.in_use -= bytes;
        arena.use_in_use[use] -= bytes;
        auto it = arena.free.emplace(offset, bytes).first;
        if (auto next = std::next(it); next != arena.free.end() && it->first + it->second == next->first) {
            it->second += next->second;
            arena.free.erase(next);
        }
        if (it != arena.free.begin()) {
            if (auto previous = std::prev(it); previous->first + previous->second == it->first) {
                previous->second += it->second;
                arena.free.erase(it);
            }
        }
    }

    // The translator's output, loaded on first use.
    std::mutex library_mutex;
    bool library_tried = false;
    leonida::ShaderLibrary shader_library;

    const leonida::ShaderLibrary* library() {
        std::lock_guard lock(library_mutex);
        if (!library_tried) {
            library_tried = true;
            std::string dir = info.shader_dir;
            if (dir.empty()) {
                char buffer[1024] = {};
                size_t length = 0;
                if (getenv_s(&length, buffer, "LEONIDA_SHADERS") == 0 && length > 1) dir = buffer;
            }
            std::string error;
            if (dir.empty())
                std::fprintf(stderr, "[rt] d3d12: no translated shaders: set LEONIDA_SHADERS to <work>/shaders/spirv\n");
            else if (!shader_library.load(dir, &error))
                std::fprintf(stderr, "[rt] d3d12: %s\n", error.c_str());
            else
                std::fprintf(stderr, "[rt] d3d12: %zu translated shaders from %s\n", shader_library.size(), dir.c_str());
        }
        return shader_library.loaded() ? &shader_library : nullptr;
    }

    bool depth_usable(VkFormat format) const {
        VkFormatProperties properties{};
        format_properties(info.physical, format, &properties);
        return properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }

    // Whether the GPU has this format and usage in this sample count: not every format has every count.
    bool samples_usable(const VkImageCreateInfo& image) const {
        VkImageFormatProperties properties{};
        return image_format_properties(info.physical, image.format, image.imageType, image.tiling, image.usage,
                                       image.flags, &properties) == VK_SUCCESS &&
               (properties.sampleCounts & image.samples);
    }

    // A memory type with `required`, preferring one that also has `preferred`, in a heap large enough for the
    // allocation (leonida::memory_type_order).
    VkDeviceMemory allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred, bool device_address = false) const {
        VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        for (const std::uint32_t i :
             leonida::memory_type_order(memory, requirements.memoryTypeBits, required, preferred, requirements.size)) {
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            if (device_address) allocate.pNext = &flags_info;
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = i;
            VkDeviceMemory out = VK_NULL_HANDLE;
            if (context->vk.vkAllocateMemory(info.device, &allocate, nullptr, &out) == VK_SUCCESS) return out;
        }
        return VK_NULL_HANDLE;
    }
};

class DescriptorHeap final
    : public Child<DescriptorHeap, ID3D12DescriptorHeapStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable,
                   ID3D12DescriptorHeap> {
public:
    // {4a7d2c90-5e1b-4f63-8c0a-91d3b6e2f417}
    static constexpr GUID kTag{0x4a7d2c90, 0x5e1b, 0x4f63, {0x8c, 0x0a, 0x91, 0xd3, 0xb6, 0xe2, 0xf4, 0x17}};

    // A shader-visible heap lives in the GPU address arena: its CPU handles point at the arena's host mapping and
    // its GPU handle is the console VA, so the bytes the game or Create*View writes are what shaders load. Other
    // heaps (RTV, DSV, non-visible staging heaps) stay in host memory.
    DescriptorHeap(DeviceState& device, const D3D12_DESCRIPTOR_HEAP_DESC& desc) : device_(device), desc_(desc) {
        const VkDeviceSize bytes = VkDeviceSize(desc.NumDescriptors) * kDescriptorSize;
        if ((desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && device.arena.mapped &&
            device.arena_allocate(bytes, 256, arena_offset_, DeviceState::arena_descriptor_heap)) {
            in_arena_ = true;
            cpu_ = device.arena.mapped + arena_offset_;
            std::memset(cpu_, 0, std::size_t(bytes));
        } else {
            storage_.reset(new std::byte[std::size_t(bytes)]());
            cpu_ = reinterpret_cast<std::uint8_t*>(storage_.get());
        }
    }

    ~DescriptorHeap() override {
        if (in_arena_) device_.arena_release(arena_offset_, VkDeviceSize(desc_.NumDescriptors) * kDescriptorSize,
                                            DeviceState::arena_descriptor_heap);
    }

    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override {
        return {reinterpret_cast<SIZE_T>(cpu_)};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override {
        if (!(desc_.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return {0};
        if (!in_arena_) log_once("shader-visible heap without a GPU address arena; flags", int(desc_.Flags));
        return {in_arena_ ? UINT64(arena_offset_) : 0};
    }

private:
    DeviceState& device_;
    D3D12_DESCRIPTOR_HEAP_DESC desc_;
    std::unique_ptr<std::byte[]> storage_;
    std::uint8_t* cpu_ = nullptr;
    VkDeviceSize arena_offset_ = 0;
    bool in_arena_ = false;
};

class Resource final
    : public Child<Resource, ID3D12ResourceStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Resource> {
public:
    // {9b3e5f12-7c48-4d0e-a6b1-2f8d4c7e9a35}
    static constexpr GUID kTag{0x9b3e5f12, 0x7c48, 0x4d0e, {0xa6, 0xb1, 0x2f, 0x8d, 0x4c, 0x7e, 0x9a, 0x35}};

    Resource(DeviceState& device, const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_PROPERTIES& heap,
             D3D12_HEAP_FLAGS heap_flags)
        : device_(device), desc_(desc), heap_(heap), heap_flags_(heap_flags) {}

    ~Resource() override {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        device_.context->remove_resource(id());
        if (in_arena_) device_.arena_release(arena_offset_, desc_.Width, DeviceState::arena_buffer);
        if (image_) vk.vkDestroyImage(dev, image_, nullptr);
        if (buffer_) vk.vkDestroyBuffer(dev, buffer_, nullptr);
        if (memory_) vk.vkFreeMemory(dev, memory_, nullptr);
    }

    leonida::ResourceId id() const { return resource_id(this); }
    VkImage image() const { return image_; }

    // Default-heap buffers are ranges of the GPU address arena: console VA = offset, zeroed through the arena's host
    // mapping. Upload and readback buffers keep their own host memory; with buffer device addresses their console
    // VA is their device address minus heap_base, so a shader's heap_base + VA still lands on them (section 3.3).
    HRESULT create_buffer() {
        auto& vk = device_.context->vk;
        const bool host = cpu_visible();
        leonida::ResourceBinding binding{};  // a buffer: no image
        binding.size = desc_.Width;
        if (!host && device_.arena.mapped) {
            if (!device_.arena_allocate(desc_.Width, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, arena_offset_,
                                        DeviceState::arena_buffer)) {
                log_once("GPU address arena full (raise LEONIDA_ARENA_MB); buffer size in KiB:",
                         int(desc_.Width / 1024));
                return E_OUTOFMEMORY;
            }
            in_arena_ = true;
            std::memset(device_.arena.mapped + arena_offset_, 0, std::size_t(desc_.Width));
            va_ = arena_offset_;
            binding.va = va_;
            binding.buffer = device_.arena.buffer;
            binding.buffer_offset = arena_offset_;
            device_.context->add_resource(id(), binding);
            return S_OK;
        }
        const bool addressable = device_.arena.buffer != VK_NULL_HANDLE;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = desc_.Width;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if (addressable) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (vk.vkCreateBuffer(device_.info.device, &info, nullptr, &buffer_) != VK_SUCCESS) return E_OUTOFMEMORY;
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(device_.info.device, buffer_, &requirements);
        memory_ = host ? device_.allocate(requirements,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          heap_.Type == D3D12_HEAP_TYPE_READBACK ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0,
                                          addressable)
                       : device_.allocate(requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, addressable);
        if (!memory_ || vk.vkBindBufferMemory(device_.info.device, buffer_, memory_, 0) != VK_SUCCESS)
            return E_OUTOFMEMORY;
        if (addressable) {
            VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            address.buffer = buffer_;
            va_ = vk.vkGetBufferDeviceAddress(device_.info.device, &address) - device_.arena.address;
            device_.log_va(heap_.Type == D3D12_HEAP_TYPE_READBACK ? "readback buffer" : "upload buffer", va_,
                           desc_.Width);
        }
        if (host) {
            if (vk.vkMapMemory(device_.info.device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_) != VK_SUCCESS)
                return E_OUTOFMEMORY;
            std::memset(mapped_, 0, std::size_t(desc_.Width));  // committed resources start zeroed
        } else {
            log_once("default-heap buffers are not zero-filled yet; dimension", int(desc_.Dimension));
        }
        binding.buffer = buffer_;
        binding.va = va_;
        device_.context->add_resource(id(), binding);
        return S_OK;
    }

    HRESULT create_texture(const Format& format) {
        auto& vk = device_.context->vk;
        const bool volume = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const UINT depth = volume ? desc_.DepthOrArraySize : 1;
        if (desc_.MipLevels == 0) {
            UINT64 size = std::max<UINT64>({desc_.Width, desc_.Height, depth});
            UINT16 mips = 1;
            while (size > 1) {
                size >>= 1;
                ++mips;
            }
            desc_.MipLevels = mips;
        }
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = format.typeless ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        info.format = format.format;
        info.extent = {std::uint32_t(desc_.Width), desc_.Height, depth};
        info.mipLevels = desc_.MipLevels;
        info.arrayLayers = volume ? 1 : desc_.DepthOrArraySize;
        const std::uint32_t samples = std::max<UINT>(1, desc_.SampleDesc.Count);
        info.samples = VkSampleCountFlagBits(samples);
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (!(desc_.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (samples > 1 && !device_.samples_usable(info)) {
            log_once("sample count not supported by the GPU for this format:", int(samples));
            return E_INVALIDARG;
        }
        if (vk.vkCreateImage(device_.info.device, &info, nullptr, &image_) != VK_SUCCESS) return E_OUTOFMEMORY;
        VkMemoryRequirements requirements{};
        vk.vkGetImageMemoryRequirements(device_.info.device, image_, &requirements);
        memory_ = device_.allocate(requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memory_ || vk.vkBindImageMemory(device_.info.device, image_, memory_, 0) != VK_SUCCESS)
            return E_OUTOFMEMORY;

        leonida::ResourceBinding binding{};
        binding.image = image_;
        binding.format = format.format;
        binding.aspects = format.aspects;
        binding.width = std::uint32_t(desc_.Width);
        binding.height = desc_.Height;
        binding.mip_levels = desc_.MipLevels;
        binding.array_layers = info.arrayLayers;
        binding.depth = depth;
        binding.volume = volume;
        binding.samples = samples;
        binding.simultaneous_access = (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
        binding.not_zeroed = (heap_flags_ & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) != 0;
        // Never used yet: the first command that touches a part zero-fills it and moves it to the layout of the
        // state that command expects (vulkan_replay.h, initialize), so no submission is needed here.
        device_.context->add_resource(id(), binding, VK_IMAGE_LAYOUT_UNDEFINED);
        return S_OK;
    }

    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }

    // Buffers only, as on D3D12; textures have no GPU VA (images are reached through descriptors).
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return va_; }

    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) override {
        if (properties) *properties = heap_;
        if (flags) *flags = heap_flags_;
        return S_OK;
    }

    // Buffers on upload and readback heaps are mapped persistently; the ranges are hints, as on D3D12.
    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE*, void** data) override {
        if (!mapped_ || subresource != 0) return E_INVALIDARG;
        if (data) *data = mapped_;
        return S_OK;
    }
    void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE*) override {}

private:
    bool cpu_visible() const {
        if (heap_.Type == D3D12_HEAP_TYPE_UPLOAD || heap_.Type == D3D12_HEAP_TYPE_READBACK) return true;
        return heap_.Type == D3D12_HEAP_TYPE_CUSTOM && heap_.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
               heap_.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    }

    DeviceState& device_;
    D3D12_RESOURCE_DESC desc_;
    D3D12_HEAP_PROPERTIES heap_;
    D3D12_HEAP_FLAGS heap_flags_;
    VkImage image_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize arena_offset_ = 0;
    bool in_arena_ = false;
    D3D12_GPU_VIRTUAL_ADDRESS va_ = 0;
};

// DXGI -> Vulkan for vertex attributes, with each element's size for D3D12_APPEND_ALIGNED_ELEMENT.
bool vertex_format(DXGI_FORMAT format, VkFormat& out, UINT& bytes) {
    struct Row {
        DXGI_FORMAT dxgi;
        VkFormat vk;
        UINT bytes;
    };
    static constexpr Row rows[] = {
        {DXGI_FORMAT_R32G32B32A32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {DXGI_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, 16},
        {DXGI_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SINT, 16},
        {DXGI_FORMAT_R32G32B32_FLOAT, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {DXGI_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_UINT, 12},
        {DXGI_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_SINT, 12},
        {DXGI_FORMAT_R32G32_FLOAT, VK_FORMAT_R32G32_SFLOAT, 8},
        {DXGI_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, 8},
        {DXGI_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SINT, 8},
        {DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, 4},
        {DXGI_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, 4},
        {DXGI_FORMAT_R32_SINT, VK_FORMAT_R32_SINT, 4},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8},
        {DXGI_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, 8},
        {DXGI_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, 8},
        {DXGI_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT, 8},
        {DXGI_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT, 8},
        {DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, 4},
        {DXGI_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UNORM, 4},
        {DXGI_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SNORM, 4},
        {DXGI_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_UINT, 4},
        {DXGI_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_SINT, 4},
        {DXGI_FORMAT_R16_FLOAT, VK_FORMAT_R16_SFLOAT, 2},
        {DXGI_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, 2},
        {DXGI_FORMAT_R16_UINT, VK_FORMAT_R16_UINT, 2},
        {DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4},
        {DXGI_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 4},
        {DXGI_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT, 4},
        {DXGI_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SINT, 4},
        {DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 4},
        {DXGI_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 2},
        {DXGI_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_UINT, 2},
        {DXGI_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1},
        {DXGI_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, 1},
        {DXGI_FORMAT_R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4},
        {DXGI_FORMAT_R10G10B10A2_UINT, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4},
        {DXGI_FORMAT_R11G11B10_FLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4},
    };
    for (const Row& row : rows)
        if (row.dxgi == format) {
            out = row.vk;
            bytes = row.bytes;
            return true;
        }
    return false;
}

VkBlendFactor blend_factor(D3D12_BLEND blend) {
    switch (blend) {
    case D3D12_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case D3D12_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case D3D12_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case D3D12_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3D12_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3D12_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case D3D12_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3D12_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case D3D12_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3D12_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3D12_BLEND_BLEND_FACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3D12_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case D3D12_BLEND_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case D3D12_BLEND_INV_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case D3D12_BLEND_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case D3D12_BLEND_INV_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case D3D12_BLEND_ALPHA_FACTOR: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case D3D12_BLEND_INV_ALPHA_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blend_op(D3D12_BLEND_OP op) {
    switch (op) {
    case D3D12_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case D3D12_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case D3D12_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
    case D3D12_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

// D3D12_COMPARISON_FUNC (NEVER = 1 .. ALWAYS = 8) and D3D12_STENCIL_OP (KEEP = 1 .. DECR = 8) list their values
// in Vulkan's order, starting at 1 instead of 0.
VkCompareOp compare_op(D3D12_COMPARISON_FUNC func) {
    const int value = int(func);
    return value >= 1 && value <= 8 ? VkCompareOp(value - 1) : VK_COMPARE_OP_ALWAYS;
}
VkStencilOp stencil_op(D3D12_STENCIL_OP op) {
    const int value = int(op);
    return value >= 1 && value <= 8 ? VkStencilOp(value - 1) : VK_STENCIL_OP_KEEP;
}

VkStencilOpState stencil_face(const D3D12_DEPTH_STENCILOP_DESC& face, UINT8 read_mask, UINT8 write_mask) {
    VkStencilOpState state{};
    state.failOp = stencil_op(face.StencilFailOp);
    state.passOp = stencil_op(face.StencilPassOp);
    state.depthFailOp = stencil_op(face.StencilDepthFailOp);
    state.compareOp = compare_op(face.StencilFunc);
    state.compareMask = read_mask;
    state.writeMask = write_mask;
    return state;
}

bool equal_semantic(const std::string& a, const char* b) {
    if (!b || a.size() != std::strlen(b)) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

class RootSignature final
    : public Child<RootSignature, ID3D12RootSignatureStubs, ID3D12Object, ID3D12DeviceChild, ID3D12RootSignature> {
public:
    // {3e9b4a61-2d7c-4f18-b5a0-c71e8d92f036}
    static constexpr GUID kTag{0x3e9b4a61, 0x2d7c, 0x4f18, {0xb5, 0xa0, 0xc7, 0x1e, 0x8d, 0x92, 0xf0, 0x36}};
    explicit RootSignature(RootArgumentLayout layout) : layout_(std::move(layout)) {}
    const RootArgumentLayout& layout() const { return layout_; }

private:
    RootArgumentLayout layout_;
};

// A pipeline state. Compute only for now: the shader is found by the SHA-256 of its FSSM program in the
// translator's index, and the FSSM blob is copied into the arena as the shader's code base.
class PipelineState final
    : public Child<PipelineState, ID3D12PipelineStateStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable,
                   ID3D12PipelineState> {
public:
    // {8d2f6c14-b0e7-4a93-9c58-1f4a7e3b6d20}
    static constexpr GUID kTag{0x8d2f6c14, 0xb0e7, 0x4a93, {0x9c, 0x58, 0x1f, 0x4a, 0x7e, 0x3b, 0x6d, 0x20}};
    explicit PipelineState(DeviceState& device) : device_(device) {}

    ~PipelineState() override {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        if (registered_) device_.context->remove_pipeline(id());
        if (pipeline_.pipeline) vk.vkDestroyPipeline(dev, pipeline_.pipeline, nullptr);
        if (pipeline_.layout) vk.vkDestroyPipelineLayout(dev, pipeline_.layout, nullptr);
        if (module_) vk.vkDestroyShaderModule(dev, module_, nullptr);
        if (graphics_.pipeline) vk.vkDestroyPipeline(dev, graphics_.pipeline, nullptr);
        if (graphics_.layout) vk.vkDestroyPipelineLayout(dev, graphics_.layout, nullptr);
        for (VkShaderModule module : modules_) vk.vkDestroyShaderModule(dev, module, nullptr);
        if (code_bytes_) device_.arena_release(code_base_, code_bytes_, DeviceState::arena_shader);
    }

    // Pipeline layouts declare the whole 144-byte block as one push-constant range. A device whose
    // maxPushConstantsSize is smaller (128 bytes is the Vulkan minimum, which some Intel and mobile drivers
    // report) needs the uniform-buffer tail #224 flags with push_constants_need_fallback, which is not built yet,
    // so pipelines are refused there with the limit named instead of breaking VUID-VkPushConstantRange-size-00298.
    bool push_constants_fit() const {
        const std::uint32_t limit = device_.info.max_push_constants_size;
        if (limit == 0 || limit >= sizeof(leonida::ShaderUserData)) return true;  // 0: not queried
        log_once("pipelines need 144 bytes of push constants (uniform-buffer fallback not built yet); "
                 "maxPushConstantsSize",
                 int(limit));
        return false;
    }

    std::uintptr_t id() const { return reinterpret_cast<std::uintptr_t>(static_cast<const ID3D12PipelineState*>(this)); }
    bool compute() const { return registered_ && !is_graphics_; }
    bool graphics() const { return registered_ && is_graphics_; }

    // A VS (+ PS) pipeline for dynamic rendering. Vertex input comes from the input layout matched to the VS's
    // input signature by semantic name and index: location = the element's register, the translator's rule
    // (docs/design.md, "Vertex fetch"). Hull, domain and geometry stages, more than one sample, and two stages
    // that both read their code base are refused with a log. Viewport, scissor, topology, vertex strides, stencil
    // reference and blend factor are dynamic, set from the list's state at each draw.
    HRESULT create_graphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        if (!desc.VS.pShaderBytecode || !desc.VS.BytecodeLength) return E_INVALIDARG;
        if (!device_.arena.mapped) {
            log_once("pipelines need the GPU address arena (bufferDeviceAddress); stage", 1);
            return E_NOTIMPL;
        }
        if (!push_constants_fit()) return E_NOTIMPL;
        if (desc.HS.BytecodeLength || desc.DS.BytecodeLength || desc.GS.BytecodeLength || desc.StreamOutput.NumEntries) {
            log_once("graphics pipeline stage not supported yet (hull/domain/geometry/stream output):", 0);
            return E_NOTIMPL;
        }
        if (desc.SampleDesc.Count > 1) {
            log_once("multisampled graphics pipelines not supported yet; samples", int(desc.SampleDesc.Count));
            return E_NOTIMPL;
        }
        const leonida::ShaderLibrary* library = device_.library();
        if (!library) return E_FAIL;

        struct Stage {
            const char* name;
            VkShaderStageFlagBits bit;
            const D3D12_SHADER_BYTECODE& code;
            const leonida::ShaderEntry* entry = nullptr;
            std::vector<std::uint8_t> fssm;
        };
        Stage stages[2] = {{"vs", VK_SHADER_STAGE_VERTEX_BIT, desc.VS}, {"ps", VK_SHADER_STAGE_FRAGMENT_BIT, desc.PS}};
        std::vector<VkPipelineShaderStageCreateInfo> stage_infos;
        const Stage* code_stage = nullptr;
        for (Stage& stage : stages) {
            if (!stage.code.pShaderBytecode || !stage.code.BytecodeLength) continue;  // no PS: depth only
            stage.fssm = leonida::fssm_program(
                {static_cast<const std::uint8_t*>(stage.code.pShaderBytecode), stage.code.BytecodeLength});
            if (stage.fssm.empty()) {
                log_once("shader bytecode holds no FSSM program; bytes", int(stage.code.BytecodeLength));
                return E_INVALIDARG;
            }
            const std::string hash = leonida::sha256_hex(stage.fssm);
            stage.entry = library->find(stage.name, hash);
            if (!stage.entry) {
                std::fprintf(stderr, "[rt] d3d12: no translated %s for FSSM sha256 %s\n", stage.name, hash.c_str());
                return E_INVALIDARG;
            }
            if (stage.entry->binding_count) {
                std::fprintf(stderr, "[rt] d3d12: %s uses set-0 bindings; the bindless set is not implemented yet\n",
                             stage.entry->name.c_str());
                return E_NOTIMPL;
            }
            if (stage.entry->uses_code_base) {
                if (code_stage) {
                    log_once("two stages read their code base; one push-constant slot holds one (question 14)", 0);
                    return E_NOTIMPL;
                }
                code_stage = &stage;
            }
            const std::vector<std::uint32_t> words = library->spirv(*stage.entry);
            if (words.empty()) return E_FAIL;
            VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module_info.codeSize = words.size() * 4;
            module_info.pCode = words.data();
            VkShaderModule module = VK_NULL_HANDLE;
            if (vk.vkCreateShaderModule(dev, &module_info, nullptr, &module) != VK_SUCCESS) return E_FAIL;
            modules_.push_back(module);
            VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            info.stage = stage.bit;
            info.module = module;
            info.pName = "main";
            stage_infos.push_back(info);
        }

        // Input layout -> vertex input, by semantic.
        std::vector<VkVertexInputAttributeDescription> attributes;
        std::vector<VkVertexInputBindingDescription> bindings;
        UINT slot_end[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& element = desc.InputLayout.pInputElementDescs[i];
            if (element.InputSlot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return E_INVALIDARG;
            VkFormat format;
            UINT bytes;
            if (!vertex_format(element.Format, format, bytes)) {
                log_once("vertex attribute format not supported yet:", element.Format);
                return E_INVALIDARG;
            }
            const UINT offset = element.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT
                                    ? (slot_end[element.InputSlot] + 3) / 4 * 4
                                    : element.AlignedByteOffset;
            slot_end[element.InputSlot] = offset + bytes;
            const leonida::SignatureElement* input = nullptr;
            for (const auto& e : stages[0].entry->input_signature)
                if (e.system_value == 0 && e.semantic_index == element.SemanticIndex &&
                    equal_semantic(e.semantic, element.SemanticName))
                    input = &e;
            if (!input) continue;  // the shader does not read this element
            attributes.push_back({input->register_index, element.InputSlot, format, offset});
            const bool instance = element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
            if (instance && element.InstanceDataStepRate > 1)
                log_once("instance step rates above 1 are not modelled; rate", int(element.InstanceDataStepRate));
            if (!(graphics_.vertex_slots & (1u << element.InputSlot))) {
                graphics_.vertex_slots |= 1u << element.InputSlot;
                bindings.push_back({element.InputSlot, 0,  // stride: dynamic, from IASetVertexBuffers
                                    instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
            }
        }
        for (const auto& e : stages[0].entry->input_signature) {
            if (e.system_value) continue;
            const bool fed = std::any_of(attributes.begin(), attributes.end(),
                                         [&](const auto& a) { return a.location == e.register_index; });
            if (!fed) {  // D3D12 rejects an input layout that leaves a shader input unfed
                std::fprintf(stderr, "[rt] d3d12: the input layout does not feed %s%u (register %u)\n",
                             e.semantic.c_str(), e.semantic_index, e.register_index);
                return E_INVALIDARG;
            }
        }
        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex_input.vertexBindingDescriptionCount = std::uint32_t(bindings.size());
        vertex_input.pVertexBindingDescriptions = bindings.data();
        vertex_input.vertexAttributeDescriptionCount = std::uint32_t(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        switch (desc.PrimitiveTopologyType) {
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: assembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE: assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        default:
            log_once("primitive topology type not supported:", desc.PrimitiveTopologyType);
            return E_INVALIDARG;
        }
        if (desc.IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED)
            log_once("index-buffer strip cuts (primitive restart) are not modelled; value", desc.IBStripCutValue);

        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        const D3D12_RASTERIZER_DESC& r = desc.RasterizerState;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = r.FillMode == D3D12_FILL_MODE_WIREFRAME ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        raster.cullMode = r.CullMode == D3D12_CULL_MODE_FRONT  ? VK_CULL_MODE_FRONT_BIT
                          : r.CullMode == D3D12_CULL_MODE_BACK ? VK_CULL_MODE_BACK_BIT
                                                               : VK_CULL_MODE_NONE;
        // With the negative-height viewport, framebuffer winding matches D3D12's screen-space winding.
        raster.frontFace = r.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        raster.depthBiasEnable = r.DepthBias || r.SlopeScaledDepthBias != 0.0f;
        raster.depthBiasConstantFactor = float(r.DepthBias);
        raster.depthBiasClamp = r.DepthBiasClamp;
        raster.depthBiasSlopeFactor = r.SlopeScaledDepthBias;
        raster.lineWidth = 1.0f;
        if (!r.DepthClipEnable) log_once("DepthClipEnable = FALSE is not modelled (depth clamp); flag", 0);

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        const VkSampleMask sample_mask = desc.SampleMask;
        multisample.pSampleMask = &sample_mask;
        multisample.alphaToCoverageEnable = desc.BlendState.AlphaToCoverageEnable;

        const D3D12_DEPTH_STENCIL_DESC& ds = desc.DepthStencilState;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = ds.DepthEnable;
        depth.depthWriteEnable = ds.DepthEnable && ds.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
        depth.depthCompareOp = compare_op(ds.DepthFunc);
        depth.stencilTestEnable = ds.StencilEnable;
        depth.front = stencil_face(ds.FrontFace, ds.StencilReadMask, ds.StencilWriteMask);
        depth.back = stencil_face(ds.BackFace, ds.StencilReadMask, ds.StencilWriteMask);

        std::vector<VkPipelineColorBlendAttachmentState> blends(desc.NumRenderTargets);
        for (UINT i = 0; i < desc.NumRenderTargets; ++i) {
            const D3D12_RENDER_TARGET_BLEND_DESC& b =
                desc.BlendState.RenderTarget[desc.BlendState.IndependentBlendEnable ? i : 0];
            if (b.LogicOpEnable) log_once("logic ops are not modelled; render target", int(i));
            blends[i].blendEnable = b.BlendEnable;
            blends[i].srcColorBlendFactor = blend_factor(b.SrcBlend);
            blends[i].dstColorBlendFactor = blend_factor(b.DestBlend);
            blends[i].colorBlendOp = blend_op(b.BlendOp);
            blends[i].srcAlphaBlendFactor = blend_factor(b.SrcBlendAlpha);
            blends[i].dstAlphaBlendFactor = blend_factor(b.DestBlendAlpha);
            blends[i].alphaBlendOp = blend_op(b.BlendOpAlpha);
            blends[i].colorWriteMask = b.RenderTargetWriteMask & 0xf;  // R, G, B, A in both APIs
        }
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = std::uint32_t(blends.size());
        blend.pAttachments = blends.data();

        graphics_.color_formats.assign(desc.NumRenderTargets, VK_FORMAT_UNDEFINED);
        for (UINT i = 0; i < desc.NumRenderTargets; ++i) {
            if (desc.RTVFormats[i] == DXGI_FORMAT_UNKNOWN) continue;
            graphics_.color_formats[i] = vulkan_format(desc.RTVFormats[i], false).format;
            if (graphics_.color_formats[i] == VK_FORMAT_UNDEFINED) {
                log_once("render-target format not supported yet:", desc.RTVFormats[i]);
                return E_INVALIDARG;
            }
        }
        if (desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
            Format f = vulkan_format(desc.DSVFormat, true);
            if (f.format == VK_FORMAT_D24_UNORM_S8_UINT && !device_.depth_usable(f.format))
                f.format = VK_FORMAT_D32_SFLOAT_S8_UINT;  // as CreateCommittedResource does
            graphics_.depth_format = f.format;
            graphics_.depth_aspects = f.aspects;
        }
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = std::uint32_t(graphics_.color_formats.size());
        rendering.pColorAttachmentFormats = graphics_.color_formats.data();
        if (graphics_.depth_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) rendering.depthAttachmentFormat = graphics_.depth_format;
        if (graphics_.depth_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) rendering.stencilAttachmentFormat = graphics_.depth_format;

        static constexpr VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = std::uint32_t(std::size(dynamic_states));
        dynamic.pDynamicStates = dynamic_states;

        // Every stage this path accepts reads the user SGPRs, so the range covers the whole stage table, not only the
        // stages present. A stage added to the table (geometry, mesh) is covered without touching this.
        for (const Stage& stage : stages) graphics_.push_stages |= stage.bit;
        const VkPushConstantRange push{graphics_.push_stages, 0, sizeof(leonida::ShaderUserData)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        if (vk.vkCreatePipelineLayout(dev, &layout_info, nullptr, &graphics_.layout) != VK_SUCCESS) return E_FAIL;

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        info.stageCount = std::uint32_t(stage_infos.size());
        info.pStages = stage_infos.data();
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = graphics_.layout;
        if (vk.vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &graphics_.pipeline) != VK_SUCCESS)
            return E_FAIL;

        if (code_stage) {
            VkDeviceSize offset = 0;
            if (!device_.arena_allocate(code_stage->fssm.size(), 256, offset, DeviceState::arena_shader)) return E_OUTOFMEMORY;
            std::memcpy(device_.arena.mapped + offset, code_stage->fssm.data(), code_stage->fssm.size());
            code_bytes_ = code_stage->fssm.size();
            code_base_ = offset;
            graphics_.code_base = offset;
        }
        device_.context->add_graphics_pipeline(id(), graphics_);
        registered_ = true;
        is_graphics_ = true;
        return S_OK;
    }

    HRESULT create_compute(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc) {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        if (!desc.CS.pShaderBytecode || !desc.CS.BytecodeLength) return E_INVALIDARG;
        if (!device_.arena.mapped) {
            log_once("pipelines need the GPU address arena (bufferDeviceAddress); stage", 0);
            return E_NOTIMPL;
        }
        if (!push_constants_fit()) return E_NOTIMPL;
        const std::vector<std::uint8_t> fssm = leonida::fssm_program(
            {static_cast<const std::uint8_t*>(desc.CS.pShaderBytecode), desc.CS.BytecodeLength});
        if (fssm.empty()) {
            log_once("compute shader bytecode holds no FSSM program; bytes", int(desc.CS.BytecodeLength));
            return E_INVALIDARG;
        }
        const leonida::ShaderLibrary* library = device_.library();
        if (!library) return E_FAIL;
        const std::string hash = leonida::sha256_hex(fssm);
        const leonida::ShaderEntry* entry = library->find("cs", hash);
        if (!entry) {
            std::fprintf(stderr, "[rt] d3d12: no translated compute shader for FSSM sha256 %s\n", hash.c_str());
            return E_INVALIDARG;
        }
        if (entry->binding_count) {
            // Set 0 (bindless images, samplers, the descriptor hash table) is milestone M2.
            std::fprintf(stderr, "[rt] d3d12: %s uses set-0 bindings; the bindless set is not implemented yet\n",
                         entry->name.c_str());
            return E_NOTIMPL;
        }
        if (entry->wave64) log_once("wave64 compute shader runs 32-wide (subgroup size control not requested yet)", 0);
        const std::vector<std::uint32_t> words = library->spirv(*entry);
        if (words.empty()) {
            std::fprintf(stderr, "[rt] d3d12: %s.spv is missing or not SPIR-V\n", entry->name.c_str());
            return E_FAIL;
        }

        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = words.size() * 4;
        module_info.pCode = words.data();
        if (vk.vkCreateShaderModule(dev, &module_info, nullptr, &module_) != VK_SUCCESS) return E_FAIL;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(leonida::ShaderUserData)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        if (vk.vkCreatePipelineLayout(dev, &layout_info, nullptr, &pipeline_.layout) != VK_SUCCESS) return E_FAIL;
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = module_;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = pipeline_.layout;
        if (vk.vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_.pipeline) !=
            VK_SUCCESS)
            return E_FAIL;

        // The code base: shaders read constant tables embedded in their own program through s_getpc_b64.
        VkDeviceSize offset = 0;
        if (!device_.arena_allocate(fssm.size(), 256, offset, DeviceState::arena_shader)) return E_OUTOFMEMORY;
        std::memcpy(device_.arena.mapped + offset, fssm.data(), fssm.size());
        code_bytes_ = fssm.size();
        code_base_ = offset;
        pipeline_.code_base = offset;
        device_.context->add_pipeline(id(), pipeline_);
        registered_ = true;
        return S_OK;
    }

private:
    DeviceState& device_;
    leonida::ComputePipeline pipeline_;
    leonida::GraphicsPipeline graphics_;
    VkShaderModule module_ = VK_NULL_HANDLE;
    std::vector<VkShaderModule> modules_;
    VkDeviceSize code_base_ = 0;
    VkDeviceSize code_bytes_ = 0;
    bool registered_ = false;
    bool is_graphics_ = false;
};

// The Vulkan objects create_vulkan_device_standalone made, destroyed with the device.
struct OwnedRuntime {
    PFN_vkDestroyDevice destroy_device = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
};

class Device final
    : public Object<Device, ID3D12Device11Stubs, ID3D12Object, ID3D12Device, ID3D12Device1, ID3D12Device2,
                    ID3D12Device3, ID3D12Device4, ID3D12Device5, ID3D12Device6, ID3D12Device7, ID3D12Device8,
                    ID3D12Device9, ID3D12Device10, ID3D12Device11> {
public:
    // {c58e1d27-3a96-4b4f-9e72-6d0a8b5f3c61}
    static constexpr GUID kTag{0xc58e1d27, 0x3a96, 0x4b4f, {0x9e, 0x72, 0x6d, 0x0a, 0x8b, 0x5f, 0x3c, 0x61}};

    explicit Device(const VulkanDeviceInfo& info) {
        state_.info = info;
        const auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            info.get_instance_proc(info.instance, "vkGetDeviceProcAddr"));
        state_.format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceFormatProperties"));
        state_.image_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceImageFormatProperties"));
        const auto memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceMemoryProperties"));
        if (!get_device || !state_.format_properties || !state_.image_format_properties || !memory_properties) return;
        memory_properties(info.physical, &state_.memory);

        if (state_.info.max_push_constants_size == 0 && info.physical && info.get_instance_proc) {
            const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties"));
            if (get_props) {
                VkPhysicalDeviceProperties props{};
                get_props(info.physical, &props);
                state_.info.max_push_constants_size = props.limits.maxPushConstantsSize;
                state_.info.push_constants_need_fallback = (state_.info.max_push_constants_size < 144);
            }
        }

        state_.context = std::make_unique<leonida::VulkanContext>(get_device, info.device,
                                                                  info.separate_depth_stencil_layouts);
        if (info.buffer_device_address && state_.context->loaded() && !create_arena())
            std::fprintf(stderr, "[rt] d3d12: the GPU address arena could not be created\n");
    }

    // LEONIDA_ARENA_MB sets its size (default 256, at most 4095): it holds every default-heap buffer, shader-
    // visible descriptor heap and shader program. M0-M3 keep it host-visible (section 3.3, memory type policy).
    bool create_arena() {
        auto& vk = state_.context->vk;
        if (!vk.vkGetBufferDeviceAddress) return false;
        VkDeviceSize megabytes = 256;
        char text[16] = {};
        size_t length = 0;
        if (getenv_s(&length, text, "LEONIDA_ARENA_MB") == 0 && length > 1)
            megabytes = std::clamp<VkDeviceSize>(std::strtoull(text, nullptr, 10), 16, 4095);
        auto& arena = state_.arena;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = megabytes << 20;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (vk.vkCreateBuffer(state_.info.device, &info, nullptr, &arena.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(state_.info.device, arena.buffer, &requirements);
        arena.memory = state_.allocate(requirements,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        void* mapped = nullptr;
        if (!arena.memory || vk.vkBindBufferMemory(state_.info.device, arena.buffer, arena.memory, 0) != VK_SUCCESS ||
            vk.vkMapMemory(state_.info.device, arena.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            return false;
        arena.mapped = static_cast<std::uint8_t*>(mapped);
        VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address.buffer = arena.buffer;
        arena.address = vk.vkGetBufferDeviceAddress(state_.info.device, &address);
        arena.size = info.size;
        arena.free[64 * 1024] = arena.size - 64 * 1024;
        arena.peak_end = 64 * 1024;
        char log[4] = {};
        length = 0;
        arena.log = getenv_s(&length, log, "LEONIDA_ARENA_LOG") == 0 && length > 1 && log[0] != '0';
        state_.context->set_heap_base(arena.address);
        return true;
    }

    ~Device() override {
        if (state_.context) {
            std::lock_guard lock(state_.context->submit_mutex());
            state_.context->vk.vkDeviceWaitIdle(state_.info.device);
        }
        state_.report_arena();
        if (state_.context && state_.arena.buffer) {
            auto& vk = state_.context->vk;
            vk.vkDestroyBuffer(state_.info.device, state_.arena.buffer, nullptr);
            if (state_.arena.memory) vk.vkFreeMemory(state_.info.device, state_.arena.memory, nullptr);
        }
        state_.context.reset();
        if (owned_.device) owned_.destroy_device(owned_.device, nullptr);
        if (owned_.instance) owned_.destroy_instance(owned_.instance, nullptr);
    }

    bool ok() const { return state_.context && state_.context->loaded(); }
    const VulkanDeviceInfo& info() const { return state_.info; }
    const DeviceState& state() const { return state_; }
    IUnknown* holder() { return identity(); }
    void own(const OwnedRuntime& owned) { owned_ = owned; }
    void set_outer(IUnknown* outer) { outer_ = outer; }

    UINT STDMETHODCALLTYPE GetNodeCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid,
                                                 void** out) override {
        if (!desc) return E_INVALIDARG;
        // Copy and compute queues share the one graphics VkQueue for now (section 8.2, question 12).
        if (desc->Type != D3D12_COMMAND_LIST_TYPE_DIRECT) log_once("command queue type on the direct VkQueue:", desc->Type);
        return create_command_queue(*state_.context, state_.info.queue, state_.info.queue_family, *desc, riid, out,
                                    identity());
    }

    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out) override {
        return create_command_allocator(type, riid, out, identity());
    }

    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
                                                ID3D12PipelineState* state, REFIID riid, void** out) override {
        HRESULT hr = create_command_list(type, allocator, riid, out, identity());
        // The initial pipeline state is set as SetPipelineState would set it.
        if (SUCCEEDED(hr) && state) {
            ID3D12GraphicsCommandList* list = nullptr;
            if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list)))) {
                list->SetPipelineState(state);
                list->Release();
            }
        }
        return hr;
    }

    // Device4: a list created closed, without an allocator.
    HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS,
                                                 REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        ID3D12CommandAllocator* allocator = nullptr;
        HRESULT hr = create_command_allocator(type, IID_PPV_ARGS(&allocator), identity());
        if (FAILED(hr)) return hr;
        ID3D12GraphicsCommandList* list = nullptr;
        hr = create_command_list(type, allocator, IID_PPV_ARGS(&list), identity());
        allocator->Release();
        if (FAILED(hr)) return hr;
        list->Close();
        hr = list->QueryInterface(riid, out);
        list->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** out) override {
        return create_fence(*state_.context, initial, flags, riid, out, identity());
    }

    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid,
                                                   void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!desc || !desc->NumDescriptors) return E_INVALIDARG;
        auto* heap = new DescriptorHeap(state_, *desc);
        heap->attach_device(identity());
        return hand_out(heap, riid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT, const void* blob, SIZE_T size, REFIID riid,
                                                  void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!blob || !size) return E_INVALIDARG;
        // The repository's root-signature model (runtime/src/root_signature.h): parse the RTS0 blob, then pack
        // the parameters into the 32 user-data dwords. pack() refuses what it cannot place (more than 32 dwords:
        // the console spills those to memory, in a layout that is still open), and so does this.
        const rt::rootsig::DecodeResult parsed =
            rt::rootsig::parse_rts0(static_cast<const std::uint8_t*>(blob), size);
        if (!parsed.ok) {
            std::fprintf(stderr, "[rt] d3d12: root signature not understood: %s\n", parsed.error.c_str());
            return E_INVALIDARG;
        }
        const rt::rootsig::PackResult packed = rt::rootsig::pack(parsed.sig);
        if (!packed.ok) {
            std::fprintf(stderr, "[rt] d3d12: root signature refused: %s\n", packed.error.c_str());
            return E_INVALIDARG;
        }
        RootArgumentLayout layout;
        for (std::size_t i = 0; i < parsed.sig.params.size(); ++i) {
            std::uint32_t first = UINT32_MAX;
            for (std::uint32_t d = 0; d < packed.dword_count; ++d)
                if (packed.provenance[d].param_index == i) {
                    first = d;
                    break;
                }
            layout.parameters.push_back({parsed.sig.params[i].type, first, parsed.sig.params[i].num_values});
        }
        auto* signature = new RootSignature(std::move(layout));
        signature->attach_device(identity());
        return hand_out(signature, riid, out);
    }

    // PC slot 11; the Xbox ABI reaches it through device slot 62 (the proxy forwards).
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid,
                                                         void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!desc) return E_INVALIDARG;
        auto* pipeline = new PipelineState(state_);
        pipeline->attach_device(identity());
        if (const HRESULT hr = pipeline->create_compute(*desc); FAILED(hr)) {
            pipeline->Release();
            return hr;
        }
        return hand_out(pipeline, riid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid,
                                                          void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!desc) return E_INVALIDARG;
        auto* pipeline = new PipelineState(state_);
        pipeline->attach_device(identity());
        if (const HRESULT hr = pipeline->create_graphics(*desc); FAILED(hr)) {
            pipeline->Release();
            return hr;
        }
        return hand_out(pipeline, riid, out);
    }

    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override {
        return kDescriptorSize;
    }

    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        if (!destination.ptr) return;
        leonida::TargetView view{};
        if (const Resource* r = Resource::from<Resource>(resource)) {
            const D3D12_RESOURCE_DESC rd = const_cast<Resource*>(r)->GetDesc();
            view = {r->id(), 0, 0, rd.DepthOrArraySize};
            if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) {
                view = {r->id(), desc->Texture2D.MipSlice, 0, 1};
            } else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMS) {
                view = {r->id(), 0, 0, 1};
            } else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) {
                view = {r->id(), desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice,
                        desc->Texture2DArray.ArraySize};
            } else if (desc) {
                log_once("render-target view dimension not supported:", desc->ViewDimension);
                view = {};
            }
        } else if (resource) {
            stub("ID3D12Device::CreateRenderTargetView(resource from another device)");
        }
        std::memcpy(reinterpret_cast<void*>(destination.ptr), &view, sizeof(view));
    }

    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        if (!destination.ptr) return;
        leonida::TargetView view{};
        if (const Resource* r = Resource::from<Resource>(resource)) {
            const D3D12_RESOURCE_DESC rd = const_cast<Resource*>(r)->GetDesc();
            view = {r->id(), 0, 0, rd.DepthOrArraySize};
            if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D) {
                view = {r->id(), desc->Texture2D.MipSlice, 0, 1};
            } else if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS) {
                view = {r->id(), 0, 0, 1};
            } else if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY) {
                view = {r->id(), desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice,
                        desc->Texture2DArray.ArraySize};
            } else if (desc) {
                log_once("depth-stencil view dimension not supported:", desc->ViewDimension);
                view = {};
            }
        } else if (resource) {
            stub("ID3D12Device::CreateDepthStencilView(resource from another device)");
        }
        std::memcpy(reinterpret_cast<void*>(destination.ptr), &view, sizeof(view));
    }

    // A descriptor is its slot's bytes (a TargetView in RTV and DSV heaps, otherwise what the shader loads), so a
    // copy is a byte copy. D3D12 walks the destination and source ranges together, one descriptor at a time; a
    // null size array means every range holds one descriptor. Whichever side runs out first ends the copy.
    void STDMETHODCALLTYPE CopyDescriptors(UINT destination_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
                                           const UINT* destination_sizes, UINT source_ranges,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* source_sizes,
                                           D3D12_DESCRIPTOR_HEAP_TYPE) override {
        if (!destinations || !sources) return;
        UINT d = 0, s = 0, d_at = 0, s_at = 0;  // the range on each side, and the position in it
        while (d < destination_ranges && s < source_ranges) {
            if (d_at >= (destination_sizes ? destination_sizes[d] : 1) || !destinations[d].ptr) {
                ++d;
                d_at = 0;
            } else if (s_at >= (source_sizes ? source_sizes[s] : 1) || !sources[s].ptr) {
                ++s;
                s_at = 0;
            } else {
                std::memmove(reinterpret_cast<std::uint8_t*>(destinations[d].ptr) + std::size_t(d_at++) * kDescriptorSize,
                             reinterpret_cast<const std::uint8_t*>(sources[s].ptr) + std::size_t(s_at++) * kDescriptorSize,
                             kDescriptorSize);
            }
        }
    }

    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE source,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE) override {
        if (!destination.ptr || !source.ptr) return;
        std::memmove(reinterpret_cast<void*>(destination.ptr), reinterpret_cast<const void*>(source.ptr),
                     std::size_t(count) * kDescriptorSize);
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
                                                      const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES,
                                                      const D3D12_CLEAR_VALUE*, REFIID riid, void** out) override {
        if (out) *out = nullptr;
        if (!heap || !desc) return E_INVALIDARG;
        auto* resource = new Resource(state_, *desc, *heap, heap_flags);
        resource->attach_device(identity());
        HRESULT hr = E_NOTIMPL;
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            hr = resource->create_buffer();
        } else if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            log_once("committed resource dimension not supported:", desc->Dimension);
        } else if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
                   (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
            log_once("volume attachment views not supported, flags:", int(desc->Flags));
        } else if (heap->Type == D3D12_HEAP_TYPE_UPLOAD || heap->Type == D3D12_HEAP_TYPE_READBACK) {
            hr = E_INVALIDARG;  // D3D12 places textures on default heaps unless they are row-major
        } else if (const UINT samples = desc->SampleDesc.Count;
                   samples > 1 && ((samples & (samples - 1)) || desc->MipLevels != 1 ||
                                   desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                                   !(desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))) {
            hr = E_INVALIDARG;  // D3D12: a multisampled texture is a single-mip 2D render target or depth-stencil
        } else {
            Format format = vulkan_format(desc->Format, desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            if (format.format == VK_FORMAT_D24_UNORM_S8_UINT && !state_.depth_usable(format.format)) {
                log_once("D24_UNORM_S8_UINT unavailable, using D32_SFLOAT_S8_UINT; format", desc->Format);
                format.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
            }
            if (format.format == VK_FORMAT_UNDEFINED) {
                log_once("texture format not supported yet:", desc->Format);
                hr = E_INVALIDARG;
            } else {
                hr = resource->create_texture(format);
            }
        }
        if (FAILED(hr)) {
            resource->Release();
            return hr;
        }
        if (!out) {  // a capability check: D3D12 allows a null output
            resource->Release();
            return S_FALSE;
        }
        return hand_out(resource, riid, out);
    }

    // Footprints for copies through a buffer: rows padded to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (512 for planar
    // depth-stencil), subresources placed at D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, block-compressed sizes rounded up to whole blocks, depth-stencil
    // formats as two planes. d3d12_vulkan_device_test compares every output with the PC runtime's.
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT first, UINT count, UINT64 base,
                                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows,
                                                 UINT64* row_sizes, UINT64* total) override {
        const auto fail = [&] {
            for (UINT i = 0; i < count; ++i) {
                if (layouts) std::memset(&layouts[i], 0xff, sizeof(layouts[i]));
                if (rows) rows[i] = ~0u;
                if (row_sizes) row_sizes[i] = ~0ull;
            }
            if (total) *total = ~0ull;
        };
        if (!desc) return fail();
        const auto align = [](UINT64 value, UINT64 to) { return (value + to - 1) / to * to; };
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            if (first != 0 || count != 1) return fail();
            if (layouts) layouts[0] = {base, {DXGI_FORMAT_UNKNOWN, UINT(desc->Width), 1, 1,
                                              UINT(align(desc->Width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT))}};
            if (rows) rows[0] = 1;
            if (row_sizes) row_sizes[0] = desc->Width;
            if (total) *total = desc->Width;
            return;
        }
        const bool volume = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        if (!volume && desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            log_once("GetCopyableFootprints dimension not supported:", desc->Dimension);
            return fail();
        }
        const Format format = vulkan_format(desc->Format, true);  // typeless depth families are planar either way
        UINT mips = desc->MipLevels;
        const UINT depth = volume ? desc->DepthOrArraySize : 1;
        if (!mips)
            for (UINT64 size = std::max<UINT64>({desc->Width, desc->Height, depth}); size; size >>= 1) ++mips;
        const UINT planes = (format.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? 2 : 1;
        const UINT per_plane = mips * (volume ? 1 : desc->DepthOrArraySize);
        if (!format.format || UINT64(first) + count > UINT64(per_plane) * planes) return fail();

        UINT64 offset = base, end = base;
        for (UINT i = 0; i < count; ++i) {
            const UINT index = first + i, plane = index / per_plane, mip = index % mips;
            const VkImageAspectFlags aspect =
                planes == 2 ? (plane ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT) : format.aspects;
            const leonida::BlockFormat block = leonida::block_format(format.format, aspect);
            if (!block.bytes) return fail();
            const UINT width = std::max<UINT>(1, UINT(desc->Width >> mip)), height = std::max<UINT>(1, desc->Height >> mip);
            const UINT blocks_wide = (width + block.block - 1) / block.block;
            const UINT blocks_high = (height + block.block - 1) / block.block;
            const UINT64 row_size = UINT64(blocks_wide) * block.bytes;
            // Measured against the PC runtime (d3d12_vulkan_device_test): planar depth-stencil rows are padded to
            // 512 bytes, every other format's to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256).
            const UINT pitch = UINT(align(row_size, planes == 2 ? 2 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
                                                                : D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
            offset = align(offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            DXGI_FORMAT footprint_format = desc->Format;
            if (planes == 2) footprint_format = plane ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
            const UINT slices = std::max(1u, depth >> mip);
            if (layouts)
                layouts[i] = {offset, {footprint_format, blocks_wide * block.block, blocks_high * block.block, slices, pitch}};
            if (rows) rows[i] = blocks_high;
            if (row_sizes) row_sizes[i] = row_size;
            const UINT64 total_rows = UINT64(blocks_high) * slices;
            end = offset + (total_rows - 1) * pitch + row_size;
            offset += total_rows * pitch;
        }
        if (total) *total = end - base;
    }

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT size) override {
        log_once("CheckFeatureSupport feature", feature);
        if (!data) return E_INVALIDARG;
        switch (feature) {
        case D3D12_FEATURE_FEATURE_LEVELS: {
            if (size != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) return E_INVALIDARG;
            auto* levels = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(data);
            D3D_FEATURE_LEVEL best = D3D_FEATURE_LEVEL(0);
            for (UINT i = 0; i < levels->NumFeatureLevels; ++i)
                if (levels->pFeatureLevelsRequested[i] <= D3D_FEATURE_LEVEL_12_1)
                    best = std::max(best, levels->pFeatureLevelsRequested[i]);
            if (!best) return DXGI_ERROR_UNSUPPORTED;
            levels->MaxSupportedFeatureLevel = best;
            return S_OK;
        }
        case D3D12_FEATURE_ARCHITECTURE:
        case D3D12_FEATURE_ARCHITECTURE1: {
            const bool v1 = feature == D3D12_FEATURE_ARCHITECTURE1;
            if (size != (v1 ? sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1) : sizeof(D3D12_FEATURE_DATA_ARCHITECTURE)))
                return E_INVALIDARG;
            auto* arch = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE1*>(data);
            const UINT node = arch->NodeIndex;
            if (node != 0) return E_INVALIDARG;
            // A discrete desktop GPU: not tile-based, no UMA. The console is UMA (question 11); the layer keeps
            // upload heaps host-visible, so the game's upload path still works.
            arch->TileBasedRenderer = FALSE;
            arch->UMA = FALSE;
            arch->CacheCoherentUMA = FALSE;
            if (v1) arch->IsolatedMMU = TRUE;
            return S_OK;
        }
        case D3D12_FEATURE_D3D12_OPTIONS: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) return E_INVALIDARG;
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS*>(data);
            *opts = {};
            opts->DoublePrecisionFloatShaderOps = TRUE;
            opts->OutputMergerLogicOp = TRUE;
            opts->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_NONE;
            opts->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
            opts->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;  // Bindless descriptor indexing supported
            opts->PSSpecifiedStencilRefSupported = TRUE;
            opts->TypedUAVLoadAdditionalFormats = TRUE;
            opts->ROVsSupported = TRUE;
            opts->ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
            opts->MaxGPUVirtualAddressBitsPerResource = 40;
            opts->StandardSwizzle64KBSupported = FALSE;
            opts->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
            opts->CrossAdapterRowMajorTextureSupported = FALSE;
            opts->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
            opts->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_1;
            return S_OK;
        }
        case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
            if (size != sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT)) return E_INVALIDARG;
            auto* va = static_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT*>(data);
            // The console and PC BDA support 48-bit GPU virtual addresses (section 4.1).
            va->MaxGPUVirtualAddressBitsPerResource = 40;
            va->MaxGPUVirtualAddressBitsPerProcess = 48;
            return S_OK;
        }
        case D3D12_FEATURE_FORMAT_SUPPORT: {
            if (size != sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT)) return E_INVALIDARG;
            auto* fs = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT*>(data);
            fs->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
            fs->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
            const Format fmt = vulkan_format(fs->Format, false);
            const Format depth_fmt = vulkan_format(fs->Format, true);
            const VkFormat vk_fmt = fmt.format != VK_FORMAT_UNDEFINED ? fmt.format : depth_fmt.format;
            if (vk_fmt == VK_FORMAT_UNDEFINED || !state_.format_properties) {
                return DXGI_ERROR_UNSUPPORTED;
            }
            VkFormatProperties props{};
            state_.format_properties(state_.info.physical, vk_fmt, &props);
            const VkFormatFeatureFlags optimal = props.optimalTilingFeatures;
            const VkFormatFeatureFlags buffer = props.bufferFeatures;

            D3D12_FORMAT_SUPPORT1 s1 = D3D12_FORMAT_SUPPORT1_NONE;
            D3D12_FORMAT_SUPPORT2 s2 = D3D12_FORMAT_SUPPORT2_NONE;

            if (buffer != 0) s1 |= D3D12_FORMAT_SUPPORT1_BUFFER;
            if (buffer & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) s1 |= D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER;
            if (fs->Format == DXGI_FORMAT_R16_UINT || fs->Format == DXGI_FORMAT_R32_UINT)
                s1 |= D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER;

            if (optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                      D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                      D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
            }
            if (buffer & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
            }
            if (optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
            }
            if (optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
            }
            if (optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_BLENDABLE;
            }
            if (optimal & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
            }
            if ((optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
                (buffer & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT)) {
                s1 |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
                s2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
                if ((optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) ||
                    (buffer & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT)) {
                    s2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
                }
            }
            fs->Support1 = s1;
            fs->Support2 = s2;
            return S_OK;
        }
        default:
            // Not answered yet. The log above lists every query; answers are added as the trace asks for them.
            return E_INVALIDARG;
        }
    }

private:
    IUnknown* identity() { return outer_ ? outer_ : static_cast<ID3D12Device*>(this); }

    DeviceState state_;
    OwnedRuntime owned_;
    IUnknown* outer_ = nullptr;  // the Xbox-ABI proxy, which owns a reference to this device
};

}  // namespace

HRESULT create_vulkan_device(const VulkanDeviceInfo& info, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* device = new Device(info);
    if (!device->ok()) {
        device->Release();
        return E_FAIL;
    }
    return hand_out(device, riid, out);
}

void set_device_outer(ID3D12Device* device, IUnknown* outer) {
    if (Device* d = Device::from<Device>(device)) d->set_outer(outer);
}

const RootArgumentLayout* root_signature_layout(ID3D12RootSignature* signature) {
    const RootSignature* s = RootSignature::from<RootSignature>(signature);
    return s ? &s->layout() : nullptr;
}

std::uintptr_t compute_pipeline_id(ID3D12PipelineState* pipeline) {
    const PipelineState* p = PipelineState::from<PipelineState>(pipeline);
    return p && p->compute() ? p->id() : 0;
}

std::uintptr_t graphics_pipeline_id(ID3D12PipelineState* pipeline) {
    const PipelineState* p = PipelineState::from<PipelineState>(pipeline);
    return p && p->graphics() ? p->id() : 0;
}

VkImage vulkan_image(ID3D12Resource* resource) {
    const Resource* r = Resource::from<Resource>(resource);
    return r ? r->image() : VK_NULL_HANDLE;
}

std::uint32_t device_max_push_constants_size(ID3D12Device* device) {
    if (!device) return 0;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    if (!real) real = device;
    if (auto* d = Device::from<Device>(real)) return d->info().max_push_constants_size;
    return 0;
}

bool device_push_constants_need_fallback(ID3D12Device* device) {
    if (!device) return false;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    if (!real) real = device;
    if (auto* d = Device::from<Device>(real)) return d->info().push_constants_need_fallback;
    return false;
}

WindowPresenter::WindowPresenter(IUnknown* device, std::unique_ptr<leonida::VulkanPresenter> presenter)
    : device_(device), presenter_(std::move(presenter)) {
    device_->AddRef();
}

WindowPresenter::~WindowPresenter() {
    presenter_.reset();  // before the device it runs on
    device_->Release();
}

HRESULT WindowPresenter::present(ID3D12Resource* back_buffer, UINT sync_interval) {
    if (!back_buffer) return E_INVALIDARG;
    const VkResult result = presenter_->present(resource_id(back_buffer), sync_interval > 0);
    if (result == VK_SUCCESS) return S_OK;
    std::fprintf(stderr, "[rt] d3d12: present failed (VkResult %d)\n", int(result));
    return result == VK_ERROR_DEVICE_LOST ? DXGI_ERROR_DEVICE_REMOVED : E_FAIL;
}

VkExtent2D WindowPresenter::extent() const { return presenter_->extent(); }
std::uint64_t WindowPresenter::presented() const { return presenter_->presented(); }

std::unique_ptr<WindowPresenter> create_window_presenter(ID3D12Device* device, HWND window) {
    Device* d = Device::from<Device>(device);
    if (!d || !window) return nullptr;
    const VulkanDeviceInfo& info = d->state().info;
    if (!info.presentation) {
        std::fprintf(stderr, "[rt] d3d12: the Vulkan device was created without presentation support\n");
        return nullptr;
    }
    const auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        info.get_instance_proc(info.instance, "vkCreateWin32SurfaceKHR"));
    if (!create_surface) return nullptr;
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hinstance = GetModuleHandleW(nullptr);
    surface_info.hwnd = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (create_surface(info.instance, &surface_info, nullptr, &surface) != VK_SUCCESS) return nullptr;
    auto presenter = std::make_unique<leonida::VulkanPresenter>(*d->state().context, info.get_instance_proc,
                                                                info.instance, info.physical, info.queue,
                                                                info.queue_family, surface);  // owns the surface
    if (!presenter->ok()) {
        std::fprintf(stderr, "[rt] d3d12: cannot present to this window from the device's queue family\n");
        return nullptr;
    }
    return std::make_unique<WindowPresenter>(d->holder(), std::move(presenter));
}

// Creates its own Vulkan 1.3 instance and device. LEONIDA_VK_VALIDATION=1 adds VK_LAYER_KHRONOS_validation.
HRESULT create_vulkan_device_standalone(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    static const HMODULE library = LoadLibraryA("vulkan-1.dll");
    const auto get = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                                   reinterpret_cast<void*>(GetProcAddress(library, "vkGetInstanceProcAddr")))
                             : nullptr;
    if (!get) {
        std::fprintf(stderr, "[rt] d3d12: vulkan-1.dll not found\n");
        return E_FAIL;
    }
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    const auto instance_extensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        get(nullptr, "vkEnumerateInstanceExtensionProperties"));
    std::uint32_t extension_count = 0;
    instance_extensions(nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available(extension_count);
    instance_extensions(nullptr, &extension_count, available.data());
    const auto has = [](const std::vector<VkExtensionProperties>& list, const char* name) {
        for (const auto& e : list)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    const char* surface_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    const bool surfaces = has(available, surface_extensions[0]) && has(available, surface_extensions[1]);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gta6rt";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    if (surfaces) {
        instance_info.enabledExtensionCount = 2;
        instance_info.ppEnabledExtensionNames = surface_extensions;
    }
    const char* layer = "VK_LAYER_KHRONOS_validation";
    char validation[8] = {};
    size_t length = 0;
    if (getenv_s(&length, validation, "LEONIDA_VK_VALIDATION") == 0 && length > 1 && validation[0] != '0') {
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = &layer;
    }
    OwnedRuntime owned{};
    if (create_instance(&instance_info, nullptr, &owned.instance) != VK_SUCCESS) {
        std::fprintf(stderr, "[rt] d3d12: vkCreateInstance (Vulkan 1.3) failed\n");
        return E_FAIL;
    }
    owned.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get(owned.instance, "vkDestroyInstance"));
    const auto fail = [&](const char* why) {
        std::fprintf(stderr, "[rt] d3d12: %s\n", why);
        owned.destroy_instance(owned.instance, nullptr);
        return E_FAIL;
    };

#define LEONIDA_INSTANCE(name) const auto name = reinterpret_cast<PFN_##name>(get(owned.instance, #name))
    LEONIDA_INSTANCE(vkEnumeratePhysicalDevices);
    LEONIDA_INSTANCE(vkGetPhysicalDeviceProperties);
    LEONIDA_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    LEONIDA_INSTANCE(vkGetPhysicalDeviceFeatures2);
    LEONIDA_INSTANCE(vkCreateDevice);
    LEONIDA_INSTANCE(vkGetDeviceProcAddr);
    LEONIDA_INSTANCE(vkEnumerateDeviceExtensionProperties);
#undef LEONIDA_INSTANCE
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(owned.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(owned.instance, &count, physicals.data());
    VulkanDeviceInfo info{};
    info.get_instance_proc = get;
    info.instance = owned.instance;
    info.separate_depth_stencil_layouts = true;
    VkPhysicalDeviceProperties chosen{};
    bool chosen_bc = false;
    for (VkPhysicalDevice candidate : physicals) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        const bool bc = features.features.textureCompressionBC;
        if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore ||
            !f12.separateDepthStencilLayouts || !f12.bufferDeviceAddress)
            continue;
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (std::uint32_t i = 0; i < family_count; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (!info.physical || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                info.physical = candidate;
                info.queue_family = i;
                chosen = props;
                chosen_bc = bc;
            }
            break;
        }
    }
    if (!info.physical)
        return fail("no Vulkan 1.3 GPU with dynamicRendering, synchronization2, timelineSemaphore and "
                    "separateDepthStencilLayouts");

    VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enable13.dynamicRendering = VK_TRUE;
    enable13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &enable13};
    enable12.timelineSemaphore = VK_TRUE;
    enable12.separateDepthStencilLayouts = VK_TRUE;
    enable12.bufferDeviceAddress = VK_TRUE;
    info.buffer_device_address = true;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = info.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enable{};
    enable.textureCompressionBC = chosen_bc ? VK_TRUE : VK_FALSE;  // the game's textures are BC1-7
    if (!chosen_bc) std::fprintf(stderr, "[rt] d3d12: the GPU has no BC texture support\n");
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enable12};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &enable;
    vkEnumerateDeviceExtensionProperties(info.physical, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> device_extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(info.physical, nullptr, &extension_count, device_extensions.data());
    const char* swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    info.presentation = surfaces && has(device_extensions, swapchain);
    if (info.presentation) {
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = &swapchain;
    }
    if (vkCreateDevice(info.physical, &device_info, nullptr, &owned.device) != VK_SUCCESS)
        return fail("vkCreateDevice failed");
    info.device = owned.device;
    owned.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(vkGetDeviceProcAddr(owned.device, "vkDestroyDevice"));
    reinterpret_cast<PFN_vkGetDeviceQueue>(vkGetDeviceProcAddr(owned.device, "vkGetDeviceQueue"))(
        owned.device, info.queue_family, 0, &info.queue);
    info.max_push_constants_size = chosen.limits.maxPushConstantsSize;
    info.push_constants_need_fallback = (info.max_push_constants_size < 144);
    std::fprintf(stderr, "[rt] d3d12: Vulkan device: %s (maxPushConstantsSize: %u bytes)\n", chosen.deviceName,
                 chosen.limits.maxPushConstantsSize);
    if (info.push_constants_need_fallback) {
        std::fprintf(stderr,
                     "[rt] d3d12: WARNING: GPU maxPushConstantsSize (%u bytes) is below the 144 bytes required by "
                     "the shader contract; uniform-buffer fallback required for the 16-byte tail\n",
                     chosen.limits.maxPushConstantsSize);
    }

    auto* device = new Device(info);
    device->own(owned);  // destroyed with the device from here on, even on failure
    if (!device->ok()) {
        device->Release();
        return E_FAIL;
    }
    return hand_out(device, riid, out);
}

}  // namespace rt::d3d12
