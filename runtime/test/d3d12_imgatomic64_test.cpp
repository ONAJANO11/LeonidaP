// Dispatch the translated cs_imgatomic64.asm through D3D12's real typed UAV path, then copy back every texel.
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;

namespace {

constexpr UINT width = 16, slices = 4;  // the 2D texture, followed by the three layers of the array
using Row = std::array<std::uint64_t, width>;
using Rows = std::array<Row, slices>;

constexpr Rows initial = {{
    {0x00000001fffffff0, 0x01234567ffffffff, 0x0000000200000001, 0x8000000000000042,
     0x1234567800000042, 0x9876543200000042, 0x0000000200000003, 0x0000000200000003,
     0, 0, 0, 0, 0, 0, 0xfffffff0fedcba98, 0xaa00000011223344},
    {0x2222222200000001, 0x2222222200000002},
    {0x3333333300000001, 0x3333333300000002},
    {0x8000000000000000, 0x0000000100000042, 0x80000000deadbeef},
}};
constexpr Rows expected = {{
    {0x0000002200000010, 0x0123456900000000, 0xffffffff00000000, 0x7fffffffffffffff,
     0xabcdef0100000007, 0x9876543200000042, 0x0000000400000004, 0x00000000ffffffff,
     initial[0][1], initial[0][2], initial[0][3], initial[0][4], initial[0][5], 0x0000000200000001,
     0x0f0f0f0000dc0098, 0xaabb000055223344},
    initial[1],
    initial[2],
    {0xffffffff00000000, 0x8000000000000000, 0x00000001dead4110, 0, 0, 0, 0, 0, initial[3][0], initial[3][1]},
}};

D3D12_TEXTURE_COPY_LOCATION buffer_copy(ID3D12Resource* buffer, D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint) {
    D3D12_TEXTURE_COPY_LOCATION copy{};
    copy.pResource = buffer;
    copy.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    copy.PlacedFootprint = footprint;
    return copy;
}

D3D12_TEXTURE_COPY_LOCATION texture_copy(ID3D12Resource* texture, UINT subresource) {
    D3D12_TEXTURE_COPY_LOCATION copy{};
    copy.pResource = texture;
    copy.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy.SubresourceIndex = subresource;
    return copy;
}

}  // namespace

int main(int argc, char** argv) {
    EXPECT(argc == 3);  // shader index directory and synthetic FSSM file
    EXPECT(_putenv_s("LEONIDA_SHADERS", argv[1]) == 0);
    ID3D12Device* inner = nullptr;
    const HRESULT created = rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&inner));
    EXPECT(SUCCEEDED(created) && inner);
    Ref<ID3D12Device> device;
    EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                      reinterpret_cast<void**>(&device.p))));
    rt::d3d12::set_device_outer(inner, device.p);
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options{};
    EXPECT(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options, sizeof(options))));
    if (!options.AtomicInt64OnTypedResourceSupported) {
        std::puts("unsupported: D3D12 OPTIONS9 has no typed 64-bit atomics");
        return 77;
    }

    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC signature{};
    signature.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    signature.Desc_1_0 = {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const auto root_blob = serialize_root_signature(signature);
    EXPECT(!root_blob.empty());
    Ref<ID3D12RootSignature> root;
    EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));
    std::ifstream file(argv[2], std::ios::binary);
    EXPECT(file.good());
    const std::vector<std::uint8_t> fssm((std::istreambuf_iterator<char>(file)), {});
    EXPECT(!fssm.empty());
    const auto code = shader_container(fssm);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.p;
    pipeline_desc.CS = {code.data(), code.size()};
    Ref<ID3D12PipelineState> pipeline;
    EXPECT(SUCCEEDED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline.p))));

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                             D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void*);
    const auto cpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p);
    const auto gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p);
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    EXPECT(cpu && gpu && step == 32);

    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width;
    texture_desc.Height = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R32G32_UINT;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Ref<ID3D12Resource> textures[2], upload, readback;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, slices> footprints{};
    constexpr UINT alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    for (UINT i = 0; i < 2; ++i) {
        texture_desc.DepthOrArraySize = i ? 3 : 1;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&textures[i].p))));
        device->GetCopyableFootprints(&texture_desc, 0, texture_desc.DepthOrArraySize,
                                      i * alignment, footprints.data() + i, nullptr, nullptr, nullptr);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32G32_UINT;
        uav.ViewDimension = i ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (i) uav.Texture2DArray = {0, 1, 2, 0};  // skip the resource's first layer
        const auto handle = D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(cpu + i * step)};
        device->CreateUnorderedAccessView(textures[i].p, nullptr, &uav, handle);
        std::uint32_t words[8];
        std::memcpy(words, reinterpret_cast<const void*>(handle.ptr), sizeof(words));
        EXPECT(rt::d3d12::bindless_hash_lookup(device.p, words));
    }
    const auto transfer_desc = buffer_desc(slices * alignment);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &transfer_desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&upload.p))));
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &transfer_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback.p))));
    std::uint8_t* mapped = nullptr;
    EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
    for (UINT i = 0; i < slices; ++i) {
        EXPECT(footprints[i].Offset == i * alignment && footprints[i].Footprint.RowPitch >= sizeof(Row));
        std::memcpy(mapped + footprints[i].Offset, initial[i].data(), sizeof(Row));
    }
    upload->Unmap(0, nullptr);

    const D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    for (UINT i = 0; i < slices; ++i) {
        const auto dst = i ? texture_copy(textures[1].p, i - 1) : texture_copy(textures[0].p, 0);
        const auto src = buffer_copy(upload.p, footprints[i]);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER barriers[2];
    for (UINT i = 0; i < 2; ++i)
        barriers[i] = transition(textures[i].p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(2, barriers);
    list->SetDescriptorHeaps(1, &heap.p);
    list->SetComputeRootSignature(root.p);
    list->SetPipelineState(pipeline.p);
    list->SetComputeRootDescriptorTable(0, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
    list->Dispatch(1, 1, 1);
    for (UINT i = 0; i < 2; ++i)
        barriers[i] = transition(textures[i].p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(2, barriers);
    for (UINT i = 0; i < slices; ++i) {
        const auto dst = buffer_copy(readback.p, footprints[i]);
        const auto src = i ? texture_copy(textures[1].p, i - 1) : texture_copy(textures[0].p, 0);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    EXPECT(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
    UINT bad = 0;
    for (UINT i = 0; i < slices; ++i) {
        Row got{};
        std::memcpy(got.data(), mapped + footprints[i].Offset, sizeof(got));
        for (UINT x = 0; x < width; ++x) {
            if (got[x] == expected[i][x]) continue;
            std::fprintf(stderr, "slice %u x=%u: got %016llx, expected %016llx\n", i, x, got[x], expected[i][x]);
            ++bad;
        }
    }
    readback->Unmap(0, nullptr);
    std::printf("D3D12 typed 64-bit image atomics: %u/%u texels exact\n", slices * width - bad, slices * width);
    return bad ? 1 : 0;
}
