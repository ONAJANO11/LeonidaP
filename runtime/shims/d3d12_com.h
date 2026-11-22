// D3D12 COM objects over the Vulkan backend (src/vulkan_backend.h): command allocator, graphics command list,
// command queue and fence. Their vtables are the Windows SDK's (ID3D12GraphicsCommandList8, ID3D12CommandQueue,
// ID3D12CommandAllocator, ID3D12Fence1); every method not implemented here is a generated stub that reports its
// name once (tools/gen_d3d12_stubs.py). The Xbox-specific slots measured for the device do not apply to these
// interfaces as far as is known; docs/d3d12_vulkan.md section 3.1 lists what is still unverified.
//
// Contracts with the rest of the layer (the device proxy that will hand these objects out):
// - an ID3D12Resource's leonida::ResourceId is its pointer value; the device registers it with the VulkanContext;
// - the CPU handle of an RTV or DSV points at a leonida::TargetView in the device's descriptor heap;
// - the CPU handle of a CBV points at a ConstantBufferView;
// - the CPU handle of an SRV points at a ShaderResourceView;
// - the CPU handle of a UAV points at an UnorderedAccessView;
// - the CPU handle of a Sampler points at a SamplerView.
#pragma once

#include <windows.h>
#include <d3d12.h>

#include "../src/vulkan_backend.h"

namespace rt::d3d12 {

// `device` (optional) is what the object's GetDevice returns; the object holds a reference to it.
HRESULT create_command_queue(leonida::VulkanContext& context, VkQueue queue, std::uint32_t queue_family,
                             const D3D12_COMMAND_QUEUE_DESC& desc, REFIID riid, void** out,
                             IUnknown* device = nullptr);
HRESULT create_command_allocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out, IUnknown* device = nullptr);
HRESULT create_command_list(D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator, REFIID riid,
                            void** out, IUnknown* device = nullptr);
HRESULT create_fence(leonida::VulkanContext& context, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid,
                     void** out, IUnknown* device = nullptr);

// The Vulkan objects a D3D12 device runs on. Vulkan 1.3 with dynamicRendering, synchronization2, timelineSemaphore
// and (when separate_depth_stencil_layouts is set) separateDepthStencilLayouts enabled.
struct VulkanDeviceInfo {
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;  // a graphics queue; every D3D12 queue of the device submits to it
    std::uint32_t queue_family = 0;
    bool separate_depth_stencil_layouts = true;
    std::uint32_t max_push_constants_size = 0;
    bool push_constants_need_fallback = false;
    // Enabled: runtimeDescriptorArray, the dynamic indexing of sampled- and storage-image arrays, and update after
    // bind for sampled images, storage images and storage buffers. The device then keeps the bindless set 0 of
    // the translated shaders (docs section 3.4), and texture views and samplers are put in it as they are
    // created. bindless_partially_bound: descriptorBindingPartiallyBound is enabled as well.
    bool bindless = false;
    bool bindless_partially_bound = false;
    // samplerAnisotropy and samplerMirrorClampToEdge enabled; without them such samplers are approximated.
    bool sampler_anisotropy = false;
    bool sampler_mirror_clamp_to_edge = false;
    // VK_EXT_conditional_rendering (with its conditionalRendering feature) is enabled: SetPredication works, and
    // buffers are created with VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT. Without it a predicated list is
    // rejected at ExecuteCommandLists.
    bool conditional_rendering = false;
    // VK_EXT_host_image_copy and its hostImageCopy feature are enabled on `device`: WriteToSubresource and
    // ReadFromSubresource on textures in CPU-visible heaps copy on the CPU instead of through a staging submit.
    bool host_image_copy = false;
};

// A Vulkan-backed ID3D12Device (PC ABI, up to ID3D12Device11) on caller-owned Vulkan objects, which must outlive
// the device and every object it created. Wrap it in rt::d3d12_xs::create_device_proxy for the Xbox ABI.
HRESULT create_vulkan_device(const VulkanDeviceInfo& info, REFIID riid, void** out);

// The same, on a Vulkan instance and device of its own (destroyed with the D3D12 device). Picks the first discrete
// GPU with the features above. LEONIDA_VK_VALIDATION=1 enables VK_LAYER_KHRONOS_validation.
HRESULT create_vulkan_device_standalone(REFIID riid, void** out);

// Tells a device from create_vulkan_device* that `outer` (the Xbox-ABI proxy that owns it) is what its children's
// GetDevice must return. No reference is taken: the outer object owns the device.
void set_device_outer(ID3D12Device* device, IUnknown* outer);

// The VkImage behind a texture created by such a device, or VK_NULL_HANDLE. For tests and the swapchain.
VkImage vulkan_image(ID3D12Resource* resource);

// Whether both resources use the requested typed, non-integer colour format for a regional average resolve.
bool color_resolve_format_matches(DXGI_FORMAT format, ID3D12Resource* dst, ID3D12Resource* src);

// The maxPushConstantsSize in bytes of the Vulkan physical device behind `device`, or 0 if not a Vulkan device.
std::uint32_t device_max_push_constants_size(ID3D12Device* device);

// True when `device`'s Vulkan physical device has maxPushConstantsSize < 144 bytes, requiring
// the uniform-buffer fallback for the 16-byte tail (heap/code base).
bool device_push_constants_need_fallback(ID3D12Device* device);

// The bindless set 0 a device with VulkanDeviceInfo::bindless keeps (vulkan_bindless.h), and its layout: what a
// pipeline whose shader declares set-0 bindings is created with and binds. Null without such a set. The set is
// brought up to date at every ExecuteCommandLists.
VkDescriptorSet bindless_set(ID3D12Device* device);
VkDescriptorSetLayout bindless_set_layout(ID3D12Device* device);

// ExecuteCommandLists returns void; this is what the last call on the queue did.
leonida::ReplayResult last_execute_result(ID3D12CommandQueue* queue);

// The front/back stencil references last recorded by ID3D12GraphicsCommandList8::OMSetFrontAndBackStencilRef on
// `list`. The values are kept for the future pipeline path, which is what would apply them. False when `list` is
// not one of this module's command lists.
bool front_back_stencil_ref(ID3D12GraphicsCommandList8* list, UINT& front, UINT& back);

inline leonida::ResourceId resource_id(const ID3D12Resource* resource) {
    return reinterpret_cast<leonida::ResourceId>(resource);
}

// GFX10 Buffer Resource Descriptor (V#): 16 bytes (4 dwords).
// Matches RDNA2 hardware ISA and tools/rdna2spirv.py (buffer_address / scalar_load).
struct BufferDescriptor {
    std::uint32_t base_lo = 0;              // dword 0: base VA [31:0]
    std::uint32_t base_hi_stride = 0;       // dword 1: base VA [47:32] | (stride[13:0] << 16)
    std::uint32_t num_records = 0;          // dword 2: number of records / byte size
    std::uint32_t dst_sel_format = 0x0FAC;  // dword 3: dst_sel [11:0] | (format[6:0] << 12)

    D3D12_GPU_VIRTUAL_ADDRESS gpu_address() const {
        return static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(base_lo) |
               (static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(base_hi_stride & 0xFFFFu) << 32);
    }
    std::uint32_t stride() const {
        return (base_hi_stride >> 16) & 0x3FFFu;
    }
    std::uint32_t format_code() const {
        return (dst_sel_format >> 12) & 0x7Fu;
    }
    std::uint32_t dst_sel() const {
        return dst_sel_format & 0x0FFFu;
    }
};
static_assert(sizeof(BufferDescriptor) == 16);

// GFX10 buffer formats (matching Mesa src/amd/registers/gfx10-rsrc.json and tools/rdna2spirv.py)
inline constexpr std::uint32_t dxgi_to_gfx10_buffer_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 77;
    case DXGI_FORMAT_R32G32B32A32_UINT:  return 75;
    case DXGI_FORMAT_R32G32B32_FLOAT:    return 74;
    case DXGI_FORMAT_R32G32_FLOAT:       return 64;
    case DXGI_FORMAT_R32G32_UINT:        return 62;
    case DXGI_FORMAT_R32_FLOAT:          return 22;
    case DXGI_FORMAT_R32_UINT:           return 20;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 67;
    case DXGI_FORMAT_R16G16_FLOAT:       return 29;
    case DXGI_FORMAT_R16_FLOAT:          return 13;
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return 25;
    default:                             return 0; // Raw / unknown / byte buffer
    }
}

inline constexpr std::uint32_t dxgi_format_stride(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 12;
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return 8;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return 4;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
        return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
        return 1;
    default:
        return 0;
    }
}

inline BufferDescriptor make_buffer_descriptor(D3D12_GPU_VIRTUAL_ADDRESS va,
                                               std::uint32_t stride,
                                               std::uint32_t num_records,
                                               DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN) {
    BufferDescriptor d{};
    d.base_lo = static_cast<std::uint32_t>(va & 0xFFFFFFFFu);
    d.base_hi_stride = static_cast<std::uint32_t>(((va >> 32) & 0xFFFFu) | ((stride & 0x3FFFu) << 16));
    d.num_records = num_records;
    const std::uint32_t fmt = dxgi_to_gfx10_buffer_format(format);
    d.dst_sel_format = 0x0FACu | ((fmt & 0x7Fu) << 12);
    return d;
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4201)  // nonstandard extension used: nameless struct/union
#endif

// A CBV captured into the descriptor heap slot: exactly a 16-byte GFX10 V# descriptor.
struct ConstantBufferView {
    union {
        BufferDescriptor buffer_desc;
        struct {
            D3D12_GPU_VIRTUAL_ADDRESS buffer_location;
            std::uint32_t size_in_bytes;
            std::uint32_t dst_sel_format;
        };
    };
    std::uint32_t pad[4] = {};
};
static_assert(sizeof(ConstantBufferView) == 32);

// An SRV captured into the descriptor heap slot (V# for buffers, T# for textures).
struct ShaderResourceView {
    union {
        // GFX10 Buffer Resource Descriptor (V#) when dimension == D3D12_SRV_DIMENSION_BUFFER
        struct {
            BufferDescriptor buffer_desc;
            leonida::ResourceId buffer_resource;
            std::uint32_t buffer_dimension;
            std::uint32_t buffer_format;
        };
        // Texture SRV descriptor metadata (T#)
        struct {
            leonida::ResourceId resource;
            std::uint32_t format;
            std::uint32_t dimension;
            std::uint32_t most_detailed_mip;
            std::uint32_t mip_levels;
            std::uint32_t first_array_slice;
            std::uint32_t array_size;
        };
    };
};
static_assert(sizeof(ShaderResourceView) == 32);

// A UAV captured into the descriptor heap slot (V# for buffers, T# for textures).
struct UnorderedAccessView {
    union {
        // GFX10 Buffer Resource Descriptor (V#) when dimension == D3D12_UAV_DIMENSION_BUFFER
        struct {
            BufferDescriptor buffer_desc;
            leonida::ResourceId buffer_resource;
            std::uint32_t buffer_dimension;
            std::uint32_t buffer_format;
        };
        // Texture UAV descriptor metadata
        struct {
            leonida::ResourceId resource;
            leonida::ResourceId counter_resource;
            std::uint32_t format;
            std::uint32_t dimension;
            std::uint32_t mip_slice;
            std::uint32_t first_array_slice;
        };
    };
};
static_assert(sizeof(UnorderedAccessView) == 32);

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// A Sampler captured into the descriptor heap slot.
struct SamplerView {
    std::uint32_t filter = 0;
    std::uint8_t address_u = 0;
    std::uint8_t address_v = 0;
    std::uint8_t address_w = 0;
    std::uint8_t comparison_func = 0;
    std::uint16_t max_anisotropy = 0;
    std::uint16_t pad = 0;
    float min_lod = 0.0f;
    float max_lod = 0.0f;
    float mip_lod_bias = 0.0f;
    std::uint32_t border_color = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(SamplerView) <= 32);

}  // namespace rt::d3d12
