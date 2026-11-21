// A Vulkan-backed ID3D12Device (PC ABI) for the Xbox-ABI device proxy (d3d12_xs_device.cpp) to forward to.
// Implemented: queues, allocators, command lists, fences, descriptor heaps, render-target and depth-stencil views,
// committed buffers and textures (2D, multisampled 2D, 3D), a few feature queries. Everything else is a generated stub that reports
// itself once. See docs/d3d12_vulkan.md section 4.1/4.2 for what each answer means and what is still open.
#include "d3d12_com.h"

#include "../src/descriptor_handles.h"  // slot arithmetic and dirty ranges (P0/P1)
#include "../src/descriptor_views.h"    // D3D12 view descriptions -> GFX10 T#/V#
#include "../src/bindless_registry.h"   // key -> bindless index
#include "../src/retirement.h"          // fence-value deferred release
#include "../src/vulkan_bindless.h"     // set 0: images, samplers, descriptor table
#include "../src/descriptor_sizes.h"    // per-type slot sizes
#include "d3d12_stubs.h"  // generated
#include "d3d12_com_object.h"
#include "d3d12_xs_device.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rt::d3d12 {
namespace {

// Slot sizes are per heap type (descriptor_sizes.h): a sampler heap no longer shares the CBV/SRV/UAV stride.
// The console's own increments are open question 4 in docs/d3d12_vulkan.md; the application reads them from the
// device, and GetDescriptorHandleIncrementSize answers per type. The RTV/DSV payload is our own TargetView.
static_assert(sizeof(leonida::TargetView) <= rt::d3d12::kDescriptorSlotBytesRtv);
static_assert(sizeof(leonida::TargetView) <= rt::d3d12::kDescriptorSlotBytesDsv);

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

class DescriptorHeap;  // defined below; DeviceState only keeps the registry

// What a device's children need from it, kept apart from the COM class so children can hold a plain pointer.
struct DeviceState {
    VulkanDeviceInfo info;
    std::unique_ptr<leonida::VulkanContext> context;
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkGetPhysicalDeviceFormatProperties format_properties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties image_format_properties = nullptr;

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

    // The first memory type with `required`, preferring one that also has `preferred`.
    VkDeviceMemory allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred) const {
        for (const VkMemoryPropertyFlags flags : {required | preferred, required}) {
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                if (!(requirements.memoryTypeBits & (1u << i))) continue;
                if ((memory.memoryTypes[i].propertyFlags & flags) != flags) continue;
                VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocate.allocationSize = requirements.size;
                allocate.memoryTypeIndex = i;
                VkDeviceMemory out = VK_NULL_HANDLE;
                if (context->vk.vkAllocateMemory(info.device, &allocate, nullptr, &out) == VK_SUCCESS) return out;
            }
        }
        return VK_NULL_HANDLE;
    }

    // Every live descriptor heap, so a CPU handle maps back to its stride and dirty range. Heaps register in their
    // constructor and unregister in their destructor; a lookup holds the lock only for the scan.
    std::mutex descriptor_heaps_mutex;
    std::vector<DescriptorHeap*> descriptor_heaps;

    // The console GPU VA space: one linear arena for now, so a resource can hand a valid address to a descriptor.
    // Reserving a real range and growing it at a frame boundary is docs/d3d12_vulkan.md section 3.3's plan.
    // Set 0 for every pipeline: images, samplers and the descriptor hash table. Created once the device exists.
    std::unique_ptr<leonida::BindlessSet> bindless;
    leonida::BindlessRegistry bindless_registry;
    leonida::RetirementQueue retirement;

    std::uint64_t next_console_va = 0x100000000ull;
    std::uint64_t allocate_va(std::uint64_t bytes) {
        const std::uint64_t aligned = (std::max<std::uint64_t>(bytes, 0x10000) + 0xFFFF) & ~std::uint64_t(0xFFFF);
        const std::uint64_t va = next_console_va;
        next_console_va += aligned;
        return va;
    }
};

class DescriptorHeap final
    : public Child<DescriptorHeap, ID3D12DescriptorHeapStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable,
                   ID3D12DescriptorHeap> {
public:
    // {4a7d2c90-5e1b-4f63-8c0a-91d3b6e2f417}
    static constexpr GUID kTag{0x4a7d2c90, 0x5e1b, 0x4f63, {0x8c, 0x0a, 0x91, 0xd3, 0xb6, 0xe2, 0xf4, 0x17}};

    DescriptorHeap(DeviceState& device, const D3D12_DESCRIPTOR_HEAP_DESC& desc)
        : device_(device), desc_(desc), stride_(rt::d3d12::descriptor_slot_bytes(desc.Type)),
          storage_(new std::byte[slot_bytes(desc)]()) {
        std::lock_guard lock(device_.descriptor_heaps_mutex);
        device_.descriptor_heaps.push_back(this);
    }

    ~DescriptorHeap() override {
        std::lock_guard lock(device_.descriptor_heaps_mutex);
        auto& heaps = device_.descriptor_heaps;
        heaps.erase(std::remove(heaps.begin(), heaps.end(), this), heaps.end());
    }

    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override {
        return {reinterpret_cast<SIZE_T>(storage_.get())};
    }
    // Shader-visible heaps get a GPU handle; the console-visible arena/VGPR mapping is P0's next step, so the host
    // pointer stays a placeholder here (docs/d3d12_vulkan.md:337-341).
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override {
        if (!(desc_.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return {0};
        return {reinterpret_cast<UINT64>(storage_.get())};
    }

    UINT stride() const { return stride_; }
    UINT slot_count() const { return desc_.NumDescriptors; }
    D3D12_DESCRIPTOR_HEAP_TYPE type() const { return desc_.Type; }
    std::uint8_t* base() const { return reinterpret_cast<std::uint8_t*>(storage_.get()); }

    // True when `ptr` is slot-aligned inside this heap; `slot` receives the slot number.
    bool slot_of(const void* ptr, std::uint32_t* slot) const {
        return rt::d3d12::slot_index(reinterpret_cast<std::uintptr_t>(ptr),
                                     reinterpret_cast<std::uintptr_t>(storage_.get()), desc_.NumDescriptors, stride_,
                                     slot);
    }
    bool contains(const void* ptr) const { return slot_of(ptr, nullptr); }

    // Marks [first, first + count) as written since the last bindless scan; copies and view creation call it.
    void mark_dirty(std::uint32_t first, std::uint32_t count) {
        if (!count || !stride_) return;
        const std::uint32_t last = std::min(desc_.NumDescriptors, first + count);
        if (last <= first) return;
        std::lock_guard lock(dirty_mutex_);
        dirty_ = rt::d3d12::merge(dirty_, rt::d3d12::slot_range(first, last - first));
    }
    rt::d3d12::DirtyRange dirty() const {
        std::lock_guard lock(dirty_mutex_);
        return dirty_;
    }
    void clear_dirty() {
        std::lock_guard lock(dirty_mutex_);
        dirty_ = {};
    }

    // Copies `count` slots of this heap into `source` (P1: CopyDescriptors goes through here so the destination
    // stride is the heap's, never the CBV/SRV/UAV default). Bounds are checked against the heap.
    bool copy_slots(std::uint32_t first, const std::uint8_t* source, std::uint32_t count) {
        std::size_t bytes = 0;
        if (!rt::d3d12::slot_run_bytes(count, stride_, &bytes) || !bytes) return false;
        const std::uint64_t offset = std::uint64_t(first) * stride_;
        if (offset + bytes > std::uint64_t(desc_.NumDescriptors) * stride_) return false;
        std::memmove(base() + offset, source, bytes);
        mark_dirty(first, count);
        return true;
    }

private:
    static std::size_t slot_bytes(const D3D12_DESCRIPTOR_HEAP_DESC& desc) {
        // CreateDescriptorHeap rejects a 0 stride before constructing, so this is never 0 in practice; the max keeps
        // the allocation non-empty for a defensive caller.
        return std::max<std::size_t>(1, std::size_t(desc.NumDescriptors) * rt::d3d12::descriptor_slot_bytes(desc.Type));
    }

    DeviceState& device_;
    D3D12_DESCRIPTOR_HEAP_DESC desc_;
    UINT stride_ = 0;
    std::unique_ptr<std::byte[]> storage_;
    mutable std::mutex dirty_mutex_;
    rt::d3d12::DirtyRange dirty_;
};

// The heap that owns a CPU descriptor handle, or nullptr. The pointer stays valid for one call: D3D12 does not
// destroy a heap while a copy or a view creation uses its handles.
inline DescriptorHeap* heap_of(DeviceState& state, const void* ptr) {
    std::lock_guard lock(state.descriptor_heaps_mutex);
    for (DescriptorHeap* heap : state.descriptor_heaps) {
        if (heap->contains(ptr)) return heap;
    }
    return nullptr;
}

class Resource final
    : public Child<Resource, ID3D12ResourceStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Resource> {
public:
    // {9b3e5f12-7c48-4d0e-a6b1-2f8d4c7e9a35}
    static constexpr GUID kTag{0x9b3e5f12, 0x7c48, 0x4d0e, {0xa6, 0xb1, 0x2f, 0x8d, 0x4c, 0x7e, 0x9a, 0x35}};

    Resource(DeviceState& device, const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_PROPERTIES& heap,
             D3D12_HEAP_FLAGS heap_flags)
        : device_(device), desc_(desc), heap_(heap), heap_flags_(heap_flags),
          console_va_(device.allocate_va(std::max<std::uint64_t>(desc.Width * std::max<UINT>(desc.Height, 1),
                                                                0x10000))) {}

    ~Resource() override {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        device_.context->remove_resource(id());
        if (image_) vk.vkDestroyImage(dev, image_, nullptr);
        if (buffer_) vk.vkDestroyBuffer(dev, buffer_, nullptr);
        if (memory_) vk.vkFreeMemory(dev, memory_, nullptr);
    }

    leonida::ResourceId id() const { return resource_id(this); }
    VkImage image() const { return image_; }
    std::uint64_t console_va() const { return console_va_; }
    UINT64 STDMETHODCALLTYPE GetGPUVirtualAddress() override { return console_va_; }

    HRESULT create_buffer() {
        auto& vk = device_.context->vk;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = desc_.Width;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if (vk.vkCreateBuffer(device_.info.device, &info, nullptr, &buffer_) != VK_SUCCESS) return E_OUTOFMEMORY;
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(device_.info.device, buffer_, &requirements);
        const bool host = cpu_visible();
        memory_ = host ? device_.allocate(requirements,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          heap_.Type == D3D12_HEAP_TYPE_READBACK ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0)
                       : device_.allocate(requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memory_ || vk.vkBindBufferMemory(device_.info.device, buffer_, memory_, 0) != VK_SUCCESS)
            return E_OUTOFMEMORY;
        if (host) {
            if (vk.vkMapMemory(device_.info.device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_) != VK_SUCCESS)
                return E_OUTOFMEMORY;
            std::memset(mapped_, 0, std::size_t(desc_.Width));  // committed resources start zeroed
        } else {
            log_once("default-heap buffers are not zero-filled yet; dimension", int(desc_.Dimension));
        }
        leonida::ResourceBinding binding{};  // a buffer: no image
        binding.buffer = buffer_;
        binding.size = desc_.Width;
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
    std::uint64_t console_va_ = 0;
    VkImage image_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
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
    }

    ~Device() override {
        if (state_.context) {
            std::lock_guard lock(state_.context->submit_mutex());
            state_.context->vk.vkDeviceWaitIdle(state_.info.device);
        }
        state_.context.reset();
        if (owned_.device) owned_.destroy_device(owned_.device, nullptr);
        if (owned_.instance) owned_.destroy_instance(owned_.instance, nullptr);
    }

    bool ok() const { return state_.context && state_.context->loaded(); }
    const VulkanDeviceInfo& info() const { return state_.info; }
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
        if (state) stub("ID3D12Device::CreateCommandList(initial pipeline state)");
        return create_command_list(type, allocator, riid, out, identity());
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
        if (!rt::d3d12::descriptor_slot_bytes(desc->Type)) return E_INVALIDARG;  // no slot size, no heap
        auto* heap = new DescriptorHeap(state_, *desc);
        heap->attach_device(identity());
        return hand_out(heap, riid, out);
    }

    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type) override {
        return rt::d3d12::descriptor_slot_bytes(type);
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

    // ---- CBV/SRV/UAV views (P2 slice): encode the GFX10 descriptor and write the heap slot ----

    // Writes an 8-dword T#/V# into a CBV/SRV/UAV heap slot and marks it dirty for the bindless scan. The view's
    // registration in bindless set 0 (VkImageView/buffer index) belongs to the #157 work; this writes what a shader
    // reads from a descriptor table.
    bool write_view(D3D12_CPU_DESCRIPTOR_HANDLE destination, const void* words, std::size_t bytes) {
        if (!destination.ptr) return false;
        DescriptorHeap* heap = heap_of(state_, reinterpret_cast<const void*>(destination.ptr));
        std::uint32_t slot = 0;
        if (!heap || heap->type() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
            !heap->slot_of(reinterpret_cast<const void*>(destination.ptr), &slot)) {
            log_once("descriptor write outside a CBV/SRV/UAV heap", 0);
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(destination.ptr), words, std::min(bytes, std::size_t(heap->stride())));
        heap->mark_dirty(slot, 1);
        return true;
    }

    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        if (!desc) return;
        leonida::gfx10::BufferWords words{};
        if (!rt::d3d12::views::encode_cbv(desc->BufferLocation, desc->SizeInBytes, words))
            log_once("constant buffer view rejected, size", int(desc->SizeInBytes));
        else
            write_view(destination, words.data(), sizeof(words));
    }

    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* resource,
                                                    const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        const Resource* r = Resource::from<Resource>(resource);
        if (!r || !desc) return;
        const D3D12_RESOURCE_DESC rd = const_cast<Resource*>(r)->GetDesc();
        const VkFormat view_format =
            vulkan_format(desc->Format == DXGI_FORMAT_UNKNOWN ? rd.Format : desc->Format, false).format;
        if (desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
            leonida::gfx10::BufferWords words{};
            const rt::d3d12::views::BufferResource buffer{r->console_va(), rd.Width};
            if (rt::d3d12::views::encode_buffer_srv(*desc, buffer, view_format, words))
                write_view(destination, words.data(), sizeof(words));
            else
                log_once("buffer SRV not encoded, format", int(desc->Format));
            return;
        }
        leonida::gfx10::ImageWords words{};
        rt::d3d12::views::TextureResource texture{};
        texture.address = r->console_va();
        texture.width = static_cast<std::uint32_t>(rd.Width);
        texture.height = rd.Height;
        texture.depth = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? rd.DepthOrArraySize : 1;
        texture.mip_levels = rd.MipLevels;
        texture.array_layers = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : rd.DepthOrArraySize;
        texture.samples = std::max(rd.SampleDesc.Count, 1u);
        if (rt::d3d12::views::encode_texture_srv(*desc, texture, view_format, words))
            write_view(destination, words.data(), sizeof(words));
        else
            log_once("texture SRV not encoded, dimension", int(desc->ViewDimension));
    }

    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* resource,
                                                     ID3D12Resource* counter_resource,
                                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        if (counter_resource) log_once("UAV counter resources are not bound yet", 0);
        const Resource* r = Resource::from<Resource>(resource);
        if (!r || !desc) return;
        const D3D12_RESOURCE_DESC rd = const_cast<Resource*>(r)->GetDesc();
        const VkFormat view_format =
            vulkan_format(desc->Format == DXGI_FORMAT_UNKNOWN ? rd.Format : desc->Format, false).format;
        if (desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
            leonida::gfx10::BufferWords words{};
            const rt::d3d12::views::BufferResource buffer{r->console_va(), rd.Width};
            if (rt::d3d12::views::encode_buffer_uav(*desc, buffer, view_format, words))
                write_view(destination, words.data(), sizeof(words));
            else
                log_once("buffer UAV not encoded, format", int(desc->Format));
            return;
        }
        leonida::gfx10::ImageWords words{};
        rt::d3d12::views::TextureResource texture{};
        texture.address = r->console_va();
        texture.width = static_cast<std::uint32_t>(rd.Width);
        texture.height = rd.Height;
        texture.depth = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? rd.DepthOrArraySize : 1;
        texture.mip_levels = rd.MipLevels;
        texture.array_layers = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : rd.DepthOrArraySize;
        texture.samples = std::max(rd.SampleDesc.Count, 1u);
        if (rt::d3d12::views::encode_texture_uav(*desc, texture, view_format, words))
            write_view(destination, words.data(), sizeof(words));
        else
            log_once("texture UAV not encoded, dimension", int(desc->ViewDimension));
    }
    // A descriptor is its slot's bytes (a TargetView in RTV and DSV heaps, otherwise what the shader loads), so a
    // copy is a byte copy. D3D12 walks the destination and source ranges together, one descriptor at a time; a
    // null size array means every range holds one descriptor. Whichever side runs out first ends the copy.
    void STDMETHODCALLTYPE CopyDescriptors(UINT destination_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
                                           const UINT* destination_sizes, UINT source_ranges,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* source_sizes,
                                           D3D12_DESCRIPTOR_HEAP_TYPE type) override {
        if (!destinations || !sources) return;
        const UINT type_stride = rt::d3d12::descriptor_slot_bytes(type);
        if (!type_stride) return;  // an unknown heap type has no slot size
        UINT d = 0, s = 0, d_at = 0, s_at = 0;  // the range on each side, and the position in it
        while (d < destination_ranges && s < source_ranges) {
            const UINT d_size = destination_sizes ? destination_sizes[d] : 1;
            const UINT s_size = source_sizes ? source_sizes[s] : 1;
            if (d_at >= d_size || !destinations[d].ptr) {
                ++d;
                d_at = 0;
                continue;
            }
            if (s_at >= s_size || !sources[s].ptr) {
                ++s;
                s_at = 0;
                continue;
            }
            DescriptorHeap* destination_heap = heap_of(state_, reinterpret_cast<const void*>(destinations[d].ptr));
            DescriptorHeap* source_heap = heap_of(state_, reinterpret_cast<const void*>(sources[s].ptr));
            std::uint32_t d_slot = 0, s_slot = 0;
            if (!destination_heap || !source_heap ||
                !destination_heap->slot_of(reinterpret_cast<const void*>(destinations[d].ptr), &d_slot) ||
                !source_heap->slot_of(reinterpret_cast<const void*>(sources[s].ptr), &s_slot)) {
                ++d;
                d_at = 0;  // a handle outside every heap: nothing to copy, advance both sides by one range
                ++s;
                s_at = 0;
                continue;
            }
            const UINT copied = rt::d3d12::copy_slots_between(
                destination_heap->base(), destination_heap->slot_count(), d_slot, source_heap->base(),
                source_heap->slot_count(), s_slot, destination_heap->stride(), std::min(d_size - d_at, s_size - s_at));
            if (!copied) {
                ++d;
                d_at = 0;
                ++s;
                s_at = 0;
                continue;
            }
            destination_heap->mark_dirty(d_slot, copied);
            d_at += copied;
            s_at += copied;
            if (d_at >= d_size) {
                ++d;
                d_at = 0;
            }
            if (s_at >= s_size) {
                ++s;
                s_at = 0;
            }
        }
    }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE source,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE type) override {
        if (!destination.ptr || !source.ptr || !count) return;
        const UINT type_stride = rt::d3d12::descriptor_slot_bytes(type);
        if (!type_stride) return;
        DescriptorHeap* destination_heap = heap_of(state_, reinterpret_cast<const void*>(destination.ptr));
        std::uint32_t d_slot = 0;
        if (destination_heap &&
            destination_heap->slot_of(reinterpret_cast<const void*>(destination.ptr), &d_slot)) {
            const UINT stride = destination_heap->stride();
            DescriptorHeap* source_heap = heap_of(state_, reinterpret_cast<const void*>(source.ptr));
            std::uint32_t s_slot = 0;
            if (source_heap && source_heap->slot_of(reinterpret_cast<const void*>(source.ptr), &s_slot)) {
                const UINT copied = rt::d3d12::copy_slots_between(
                    destination_heap->base(), destination_heap->slot_count(), d_slot, source_heap->base(),
                    source_heap->slot_count(), s_slot, stride, count);
                destination_heap->mark_dirty(d_slot, copied);
                return;
            }
            // The source is not inside a known heap: copy raw bytes with the destination heap's stride.
            const UINT n = std::min(count, destination_heap->slot_count() - d_slot);
            std::size_t bytes = 0;
            if (!rt::d3d12::slot_run_bytes(n, stride, &bytes)) return;
            std::memmove(destination_heap->base() + std::size_t(d_slot) * stride,
                         reinterpret_cast<const void*>(source.ptr), bytes);
            destination_heap->mark_dirty(d_slot, n);
            return;
        }
        // A handle outside every heap: fall back to the type stride (the slot range is unknown, so no dirty mark).
        std::size_t bytes = 0;
        if (!rt::d3d12::slot_run_bytes(count, type_stride, &bytes)) return;
        std::memmove(reinterpret_cast<void*>(destination.ptr), reinterpret_cast<const void*>(source.ptr), bytes);
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

public:
    // Set 0 and its layout for the pipeline layouts the device hands out. Null when the set could not be made.
    VkDescriptorSet bindless_set() const { return state_.bindless ? state_.bindless->set() : VK_NULL_HANDLE; }
    VkDescriptorSetLayout bindless_layout() const {
        return state_.bindless ? state_.bindless->layout() : VK_NULL_HANDLE;
    }
    leonida::BindlessSet* bindless() const { return state_.bindless.get(); }
    leonida::RetirementQueue& retirement() { return state_.retirement; }

    void create_bindless(PFN_vkGetDeviceProcAddr get_device_proc) {
        if (state_.bindless || !get_device_proc) return;
        state_.bindless = std::make_unique<leonida::BindlessSet>(get_device_proc, state_.info.device, state_.memory);
    }

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
    const auto get_device_proc =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(info.get_instance_proc(info.instance, "vkGetDeviceProcAddr"));
    device->create_bindless(get_device_proc);
    return hand_out(device, riid, out);
}

void set_device_outer(ID3D12Device* device, IUnknown* outer) {
    if (Device* d = Device::from<Device>(device)) d->set_outer(outer);
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
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gta6rt";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
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
            !f12.separateDepthStencilLayouts)
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
