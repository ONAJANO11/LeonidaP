// 64-bit typed atomics in the D3D12 layer (D3D-251): D3D12_OPTIONS9, R32G32 UAV textures and their R64_UINT view in
// set 0's r64ui binding (vulkan_bindless.h, binding 26 for 2D), on a caller-owned device with and without
// VK_EXT_shader_image_atomic_int64.
//
//   1. OPTIONS9.AtomicInt64OnTypedResourceSupported follows VulkanDeviceInfo::shader_image_int64_atomics; the
//      group-shared one and OPTIONS11.AtomicInt64OnDescriptorHeapResourceSupported stay FALSE. FORMAT_SUPPORT for
//      R32G32_UINT/SINT has no UAV atomic bit, as on the host (RTX 3060, NVIDIA 32.0.16.1714).
//   2. R32G32_UINT, _SINT and _TYPELESS UAV textures (2D and 3D, and a 2D one with a castable-format list) are
//      created, GetResourceAllocationInfo answers for each, and 2D UAVs of them (R32G32_UINT or _SINT views) get a
//      T# the hash table knows.
//   3. With the extension: on each 2D UAV, ClearUnorderedAccessViewUint seeds every texel, cs_image_atomic64.comp
//      runs 64-bit add, max, compare-exchange and exchange through binding 26 at the UAV's index, and a copy reads
//      the texture back: the R64 view must alias the R32G32 texels (R the low dword), and the returned values be the
//      seed.
//
// Synthetic data only; no game files. LEONIDA_VK_VALIDATION=1 runs it under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../src/gfx10_descriptors.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

namespace g = leonida::gfx10;

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

D3D12_RESOURCE_DESC tex_desc(D3D12_RESOURCE_DIMENSION dim, UINT64 width, UINT height, UINT16 depth, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = dim;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = depth;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc = {1, 0};
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}

rt::d3d12::VulkanDeviceInfo device_info(const vktest::Device& d, bool image_int64_atomics) {
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.bindless = d.bindless;
    info.bindless_partially_bound = d.partially_bound;
    info.image_view_min_lod = d.image_view_min_lod;
    info.host_image_copy = d.host_image_copy;
    info.features = d.enabled;
    info.shader_image_int64_atomics = image_int64_atomics;
    return info;
}

struct Options {
    BOOL typed = TRUE, group_shared = TRUE, descriptor_heap = TRUE;
};

bool query_options(ID3D12Device* device, Options& out) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 o11{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9))) ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &o11, sizeof(o11))))
        return false;
    out = {o9.AtomicInt64OnTypedResourceSupported, o9.AtomicInt64OnGroupSharedSupported,
           o11.AtomicInt64OnDescriptorHeapResourceSupported};
    return true;
}

#ifdef LEONIDA_TEST_CS_IMAGE_ATOMIC64_SPV
bool read_spirv(const char* path, std::vector<std::uint32_t>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg();
    if (size <= 0 || size % 4 != 0) return false;
    out.resize(std::size_t(size) / 4);
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(out.data()), size));
}

struct Params {
    std::uint32_t index, seed_lo, seed_hi;
};

// cs_image_atomic64.comp at bindless index `index`; the 16 returned values land in `returned`.
bool run_atomics(vktest::Device& d, VkDescriptorSet set0, VkDescriptorSetLayout set0_layout, const Params& params,
                 std::array<std::uint64_t, 16>& returned) {
    std::vector<std::uint32_t> code;
    if (!read_spirv(LEONIDA_TEST_CS_IMAGE_ATOMIC64_SPV, code)) return false;
    void* out_ptr = nullptr;
    const VkBuffer out_buf = d.buffer(sizeof(returned), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out_ptr);
    if (!out_buf) return false;
    std::memset(out_ptr, 0xcd, sizeof(returned));

    const auto create_dsl = d.fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    const auto destroy_dsl = d.fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    const auto create_pool = d.fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool");
    const auto destroy_pool = d.fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    const auto alloc_sets = d.fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    const auto update_sets = d.fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
    const auto create_layout = d.fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
    const auto destroy_layout = d.fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    const auto create_mod = d.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule");
    const auto destroy_mod = d.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    const auto create_pipe = d.fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    const auto destroy_pipe = d.fn<PFN_vkDestroyPipeline>("vkDestroyPipeline");

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dsl_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl_info.bindingCount = 1;
    dsl_info.pBindings = &binding;
    VkDescriptorSetLayout set1_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto cleanup = [&](bool ok) {
        if (pipeline) destroy_pipe(d.device, pipeline, nullptr);
        if (module) destroy_mod(d.device, module, nullptr);
        if (pipe_layout) destroy_layout(d.device, pipe_layout, nullptr);
        if (pool) destroy_pool(d.device, pool, nullptr);
        if (set1_layout) destroy_dsl(d.device, set1_layout, nullptr);
        return ok;
    };
    if (create_dsl(d.device, &dsl_info, nullptr, &set1_layout) != VK_SUCCESS) return cleanup(false);
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (create_pool(d.device, &pool_info, nullptr, &pool) != VK_SUCCESS) return cleanup(false);
    VkDescriptorSetAllocateInfo alloc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_info.descriptorPool = pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &set1_layout;
    VkDescriptorSet set1 = VK_NULL_HANDLE;
    if (alloc_sets(d.device, &alloc_info, &set1) != VK_SUCCESS) return cleanup(false);
    const VkDescriptorBufferInfo buffer_info{out_buf, 0, sizeof(returned)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    update_sets(d.device, 1, &write, 0, nullptr);

    VkDescriptorSetLayout set_layouts[2] = {set0_layout, set1_layout};
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params)};
    VkPipelineLayoutCreateInfo pl_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl_info.setLayoutCount = 2;
    pl_info.pSetLayouts = set_layouts;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges = &range;
    if (create_layout(d.device, &pl_info, nullptr, &pipe_layout) != VK_SUCCESS) return cleanup(false);
    VkShaderModuleCreateInfo mod_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mod_info.codeSize = code.size() * sizeof(std::uint32_t);
    mod_info.pCode = code.data();
    if (create_mod(d.device, &mod_info, nullptr, &module) != VK_SUCCESS) return cleanup(false);
    VkComputePipelineCreateInfo pipe_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipe_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipe_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipe_info.stage.module = module;
    pipe_info.stage.pName = "main";
    pipe_info.layout = pipe_layout;
    if (create_pipe(d.device, VK_NULL_HANDLE, 1, &pipe_info, nullptr, &pipeline) != VK_SUCCESS) return cleanup(false);

    const bool ok = d.run([&](VkCommandBuffer cb) {
        const auto barrier = d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0,
                nullptr, 0, nullptr);
        VkDescriptorSet sets[2] = {set0, set1};
        d.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        d.fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout,
                                                                     0, 2, sets, 0, nullptr);
        d.fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(cb, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                           sizeof(params), &params);
        d.fn<PFN_vkCmdDispatch>("vkCmdDispatch")(cb, 1, 1, 1);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                0, 1, &after, 0, nullptr, 0, nullptr);
        return true;
    });
    if (ok) std::memcpy(returned.data(), out_ptr, sizeof(returned));
    return cleanup(ok);
}

// One D3D12 submission: `record` into a fresh direct list, executed and waited for.
template<class Record>
bool submit(ID3D12Device* device, Record record) {
    Ref<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Ref<ID3D12CommandAllocator> alloc;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue.p))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc.p))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p, nullptr,
                                         IID_PPV_ARGS(&list.p))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))))
        return false;
    record(list.p);
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    if (rt::d3d12::last_execute_result(queue.p) != leonida::ReplayResult::success) return false;
    return SUCCEEDED(queue->Signal(fence.p, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, nullptr));
}
#endif

}  // namespace

int main() {
    vktest::Device d;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;
    std::printf("VK_EXT_shader_image_atomic_int64 (shaderImageInt64Atomics): %s\n",
                d.shader_image_int64_atomics ? "enabled" : "not available");

    // 1. Without the extension (or on a device that has it, when the caller leaves it off): no 64-bit typed atomics.
    {
        const auto info = device_info(d, false);
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p))));
        Options options;
        EXPECT(query_options(device.p, options));
        EXPECT(!options.typed && !options.group_shared && !options.descriptor_heap);
        const D3D12_HEAP_PROPERTIES def_heap{D3D12_HEAP_TYPE_DEFAULT};
        const auto desc = tex_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, DXGI_FORMAT_R32G32_UINT);
        Ref<ID3D12Resource> tex;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                         IID_PPV_ARGS(&tex.p))));
        std::printf("without the extension: AtomicInt64OnTypedResourceSupported FALSE, R32G32_UINT UAV texture made\n");
    }

    const bool wide = d.shader_image_int64_atomics && d.bindless;
    const auto info = device_info(d, wide);
    Ref<ID3D12Device> device;
    EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p))));
    Options options;
    EXPECT(query_options(device.p, options));
    EXPECT(bool(options.typed) == wide && !options.group_shared && !options.descriptor_heap);
    std::printf("OPTIONS9: AtomicInt64OnTypedResourceSupported %d, AtomicInt64OnGroupSharedSupported %d; OPTIONS11: "
                "AtomicInt64OnDescriptorHeapResourceSupported %d\n",
                options.typed, options.group_shared, options.descriptor_heap);
    for (const DXGI_FORMAT format : {DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_SINT}) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{format};
        EXPECT(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))));
        EXPECT((fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0);
        constexpr UINT atomic_bits = D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
                                     D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
                                     D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
                                     D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
                                     D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
        EXPECT((fs.Support2 & atomic_bits) == 0);
    }

    // 2. The resources: 2D and 3D R32G32 UAV textures of the three formats, and a castable 2D one.
    const D3D12_HEAP_PROPERTIES def_heap{D3D12_HEAP_TYPE_DEFAULT};
    struct Texture {
        DXGI_FORMAT format;      // the resource's
        DXGI_FORMAT view;        // its UAV's
        bool castable;
        Ref<ID3D12Resource> resource;
        g::ImageWords words{};
    };
    std::array<Texture, 4> textures{{{DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_UINT, false, {}, {}},
                                     {DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32_SINT, false, {}, {}},
                                     {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT, false, {}, {}},
                                     {DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_UINT, true, {}, {}}}};
    Ref<ID3D12Device10> device10;
    EXPECT(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device10.p))));
    for (auto& t : textures) {
        const auto desc = tex_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, t.format);
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
        EXPECT(allocation.SizeInBytes != UINT64_MAX && allocation.SizeInBytes >= 4 * 4 * 8);
        if (t.castable) {
            D3D12_RESOURCE_DESC1 desc1{};
            std::memcpy(&desc1, &desc, sizeof(desc));
            const DXGI_FORMAT cast[] = {DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_FLOAT};
            EXPECT(SUCCEEDED(device10->CreateCommittedResource3(&def_heap, D3D12_HEAP_FLAG_NONE, &desc1,
                                                                D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 2,
                                                                cast, IID_PPV_ARGS(&t.resource.p))));
        } else {
            EXPECT(SUCCEEDED(device->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                             IID_PPV_ARGS(&t.resource.p))));
        }
    }
    for (const DXGI_FORMAT format : {DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32_TYPELESS}) {
        const auto desc = tex_desc(D3D12_RESOURCE_DIMENSION_TEXTURE3D, 4, 4, 4, format);
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
        EXPECT(allocation.SizeInBytes != UINT64_MAX && allocation.SizeInBytes >= 4 * 4 * 4 * 8);
        Ref<ID3D12Resource> volume;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                         IID_PPV_ARGS(&volume.p))));
    }
    Ref<ID3D12DescriptorHeap> heap, gpu_heap;
    const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, UINT(textures.size())};
    const D3D12_DESCRIPTOR_HEAP_DESC gpu_hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, UINT(textures.size()),
                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap.p))));
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&gpu_hd, IID_PPV_ARGS(&gpu_heap.p))));
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu_slot = [&](ID3D12DescriptorHeap* h, UINT i) {
        return D3D12_CPU_DESCRIPTOR_HANDLE{h->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * step};
    };
    for (UINT i = 0; i < textures.size(); ++i) {
        auto& t = textures[i];
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = t.view;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(t.resource.p, nullptr, &uav, cpu_slot(heap.p, i));
        device->CreateUnorderedAccessView(t.resource.p, nullptr, &uav, cpu_slot(gpu_heap.p, i));
        std::memcpy(t.words.data(), reinterpret_cast<const void*>(cpu_slot(heap.p, i).ptr), sizeof(t.words));
        std::uint32_t key[8];
        std::memcpy(key, t.words.data(), sizeof(key));
        EXPECT(t.words[0] || t.words[1]);
        EXPECT(rt::d3d12::bindless_hash_lookup(device.p, key) != 0);
    }
    std::printf("R32G32_UINT/SINT/TYPELESS UAV textures (2D, 3D, castable 2D) made; 2D UAVs in set 0\n");

    if (!wide) {
        std::printf("SKIP the GPU part: no VK_EXT_shader_image_atomic_int64 on this device\n");
        return d.validation_errors() ? 1 : 0;
    }
#ifdef LEONIDA_TEST_CS_IMAGE_ATOMIC64_SPV
    // 3. Through binding 26: seed, atomics, read back.
    const VkDescriptorSet set0 = rt::d3d12::bindless_set(device.p);
    const VkDescriptorSetLayout set0_layout = rt::d3d12::bindless_set_layout(device.p);
    EXPECT(set0 && set0_layout);
    const D3D12_HEAP_PROPERTIES rb_heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = 4 * 256;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.SampleDesc = {1, 0};
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    constexpr std::uint32_t seed_lo = 0xfffffff8u, seed_hi = 2;
    constexpr std::uint64_t seed = std::uint64_t(seed_hi) << 32 | seed_lo;
    for (UINT n = 0; n < textures.size(); ++n) {
        auto& t = textures[n];
        // The clear writes every texel and leaves the texture in the state the bindless set reads it in (GENERAL).
        const UINT clear[4] = {seed_lo, seed_hi, 0, 0};
        EXPECT(submit(device.p, [&](ID3D12GraphicsCommandList* list) {
            ID3D12DescriptorHeap* heaps[] = {gpu_heap.p};
            list->SetDescriptorHeaps(1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = gpu_heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(n) * step;
            list->ClearUnorderedAccessViewUint(gpu, cpu_slot(heap.p, n), t.resource.p, clear, 0, nullptr);
        }));
        std::uint32_t key[8];
        std::memcpy(key, t.words.data(), sizeof(key));
        const Params params{rt::d3d12::bindless_hash_lookup(device.p, key), seed_lo, seed_hi};
        std::array<std::uint64_t, 16> returned{};
        EXPECT(run_atomics(d, set0, set0_layout, params, returned));
        Ref<ID3D12Resource> rb;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&rb.p))));
        EXPECT(submit(device.p, [&](ID3D12GraphicsCommandList* list) {
            D3D12_RESOURCE_BARRIER to_src{};
            to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_src.Transition = {t.resource.p, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
            list->ResourceBarrier(1, &to_src);
            D3D12_TEXTURE_COPY_LOCATION dst{rb.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = {0, {t.format, 4, 4, 1, 256}};
            D3D12_TEXTURE_COPY_LOCATION src{t.resource.p, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            std::swap(to_src.Transition.StateBefore, to_src.Transition.StateAfter);
            list->ResourceBarrier(1, &to_src);
        }));
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(rb->Map(0, nullptr, &mapped)));
        std::array<std::uint64_t, 16> texels{};
        for (UINT y = 0; y < 4; ++y)
            std::memcpy(&texels[y * 4], static_cast<const std::uint8_t*>(mapped) + y * 256, 4 * sizeof(std::uint64_t));
        rb->Unmap(0, nullptr);
        for (std::uint64_t i = 0; i < 16; ++i) {
            const std::uint64_t want = i < 4    ? seed + (std::uint64_t(1) << 32) + 0x10 + i   // add: the carry too
                                       : i < 8  ? (5 + i) << 32                                 // unsigned max
                                       : i < 12 ? std::uint64_t(0xdeadbeef) << 32 | i           // compare-exchange
                                                : i * 0x0101010101010101ull;                    // exchange
            if (texels[i] != want || returned[i] != seed) {
                std::fprintf(stderr, "texture %u texel %llu: 0x%016llx (want 0x%016llx), returned 0x%016llx\n", n,
                             static_cast<unsigned long long>(i), static_cast<unsigned long long>(texels[i]),
                             static_cast<unsigned long long>(want), static_cast<unsigned long long>(returned[i]));
                return 1;
            }
        }
        std::printf("format %d (UAV %d%s): 16/16 texels and returned values exact through binding 26, index %u\n",
                    int(t.format), int(t.view), t.castable ? ", castable" : "", params.index);
    }
#else
    std::printf("SKIP the GPU part: glslangValidator was not found at configure time\n");
#endif
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u Vulkan validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("d3d12_image_atomic64_test: OK (0 validation errors)\n");
    return 0;
}
