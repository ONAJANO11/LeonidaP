// ExecuteIndirect with multi-argument command signatures (docs/d3d12_vulkan.md section 4.7), through the D3D12 API on the
// proxy only (as the M2 to M5 tests do), with translator-output shaders (hand-written RDNA2 in test/shaders: vs_ma and
// ps_ma for the draws, cs_ma for the dispatches) and every result compared with a CPU reference (error 0).
//
// (a) Draws. The signature {CONSTANT (root parameter 0, five values), VERTEX_BUFFER_VIEW (slot 0), INDEX_BUFFER_VIEW,
//     DRAW_INDEXED}, stride 80. Each record sets the root constants (a cell's offset and a colour), points the vertex
//     buffer at one of two buffers (a view that starts at vertex 4 and a negative BaseVertexLocation for odd records) and
//     the index buffer at a 16-bit or a 32-bit one, and draws one of four quads from them: 16 cells of a 4x4 grid. Four
//     calls: 8 records, then 5 of 8 by a count buffer, then count 0 (nothing), then count 100 above MaxCommandCount 2.
// (b) Dispatches. The signature {CONSTANT (root parameter 0, four values), UNORDERED_ACCESS_VIEW (root parameter 1),
//     DISPATCH}, stride 36: each record writes ((g + a) ^ b) + c - d into its own region of one buffer through the root
//     UAV, with 1 to 3 groups. Two calls: 4 records, then 3 of 4 by a count buffer.
// (c) CreateCommandSignature against the host D3D12 runtime: the same descriptions on both, success or failure alike.
// Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation (0 errors required).
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef LEONIDA_TEST_MA_DIR
#error "LEONIDA_TEST_MA_DIR must name the folder build.py wrote (spirv/index.json, fssm/*.fssm)"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

using Pixel = std::array<std::uint8_t, 4>;
constexpr Pixel clear_pixel{255, 0, 255, 255};
constexpr UINT target_size = 64;

struct Image {
    std::vector<Pixel> pixels = std::vector<Pixel>(std::size_t(target_size) * target_size, clear_pixel);
    Pixel& at(UINT x, UINT y) { return pixels[std::size_t(y) * target_size + x]; }
};

// The quads of the two vertex buffers, in sixteenths of the clip range: width and height of quad q of buffer v.
constexpr UINT quad_size[2][4][2] = {{{4, 4}, {6, 3}, {3, 6}, {5, 5}}, {{2, 2}, {8, 4}, {4, 8}, {7, 7}}};

// What record r of the draw signature says; the test writes it into the argument buffer and the reference draws it.
struct DrawRecord {
    unsigned cell, quad, vertex_buffer, index_buffer, vertex_offset;
    std::uint8_t r, g, b;
};

DrawRecord draw_record(unsigned r) {
    DrawRecord d{};
    d.cell = r;
    d.quad = (r * 3 + 1) % 4;
    d.vertex_buffer = r % 2;
    d.index_buffer = (r / 2) % 2;
    d.vertex_offset = (r % 2 && d.quad > 0) ? 4 : 0;  // the view starts at this vertex, BaseVertexLocation undoes it
    d.r = std::uint8_t(30 + 13 * r);
    d.g = std::uint8_t(200 - 11 * r);
    d.b = std::uint8_t(40 + 9 * r);
    return d;
}

// The pixels of a quad: 1/16 of the clip range is two pixels; the cell is 16 pixels, y up from the bottom.
void paint(Image& image, const DrawRecord& d) {
    const UINT w = quad_size[d.vertex_buffer][d.quad][0] * 2, h = quad_size[d.vertex_buffer][d.quad][1] * 2;
    const UINT x0 = (d.cell % 4) * 16, bottom = target_size - (d.cell / 4) * 16;
    for (UINT y = bottom - h; y < bottom; ++y)
        for (UINT x = x0; x < x0 + w; ++x) image.at(x, y) = {d.r, d.g, d.b, 255};
}

unsigned compare(const char* what, const std::uint8_t* got, UINT pitch, Image& want) {
    unsigned mismatches = 0;
    for (UINT y = 0; y < target_size; ++y)
        for (UINT x = 0; x < target_size; ++x) {
            const std::uint8_t* g = got + std::size_t(y) * pitch + x * 4;
            const Pixel& w = want.at(x, y);
            if (std::memcmp(g, w.data(), 4) == 0) continue;
            if (mismatches++ < 6)
                std::fprintf(stderr, "%s pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", what, x, y, g[0], g[1],
                             g[2], g[3], w[0], w[1], w[2], w[3]);
        }
    return mismatches;
}

// The dispatch record r: constants, which region it writes and how many groups.
struct DispatchRecord {
    std::uint32_t a, b, c, d, groups;
};
DispatchRecord dispatch_record(unsigned r) {
    return {r * 7 + 1, 0x55aa00u ^ (r * 0x101u), r * 1000, r, 1 + r % 3};
}
constexpr UINT region_bytes = 1024;

// One CreateCommandSignature case of (c).
struct SignatureCase {
    const char* name;
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
    UINT stride;
    bool root;
};

D3D12_INDIRECT_ARGUMENT_DESC argument(D3D12_INDIRECT_ARGUMENT_TYPE type, UINT index = 0, UINT dest = 0, UINT count = 0) {
    D3D12_INDIRECT_ARGUMENT_DESC a{};
    a.Type = type;
    switch (type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: a.VertexBuffer.Slot = index; break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
        a.Constant = {index, dest, count};
        break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: a.ConstantBufferView.RootParameterIndex = index; break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: a.ShaderResourceView.RootParameterIndex = index; break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: a.UnorderedAccessView.RootParameterIndex = index; break;
    default: break;
    }
    return a;
}

}  // namespace

int main() {
    vktest::Device d;
    d.request_buffer_device_address = true;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;
    if (device_status != vktest::DeviceStatus::ready) return 1;
    if (!d.bindless) {
        std::printf("SKIP: the device lacks the descriptor indexing features of the bindless set 0\n");
        return 77;
    }
    if (!d.indirect_commands) {
        std::printf("SKIP: the device lacks VK_EXT_device_generated_commands (multi-argument signatures)\n");
        return 77;
    }

    const std::filesystem::path root_dir = LEONIDA_TEST_MA_DIR;
    leonida::ShaderLibrary library;
    EXPECT(library.load((root_dir / "spirv").string()));
    const auto container_of = [&](const char* name, const char* stage, std::vector<std::uint8_t>& out) {
        const auto program = read_file(root_dir / "fssm" / (std::string(name) + ".fssm"));
        if (program.size() < 4 || !library.find(stage, leonida::sha256_hex(program))) return false;
        out = shader_container(program);
        return true;
    };
    std::vector<std::uint8_t> vs, ps, cs;
    EXPECT(container_of("vs_ma", "vs", vs) && container_of("ps_ma", "ps", ps) && container_of("cs_ma", "cs", cs));

    unsigned draw_mismatches = 0, dispatch_mismatches = 0, signature_mismatches = 0, cells_drawn = 0, groups_run = 0;
    unsigned cases = 0, skipped_cases = 0;
    std::string host_name = "none";
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.bindless = d.bindless;
        info.bindless_partially_bound = d.partially_bound;
        info.image_view_min_lod = d.image_view_min_lod;
        info.multi_draw_indirect = d.multi_draw_indirect;
        info.draw_indirect_count = d.draw_indirect_count;
        info.draw_indirect_first_instance = d.draw_indirect_first_instance;
        info.max_draw_indirect_count = d.max_draw_indirect_count;
        info.indirect_commands = d.indirect_commands;
        info.indirect_properties = d.indirect_properties;
        info.shader_dir = (root_dir / "spirv").string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        UINT64 fence_value = 0;
        const auto run = [&](std::initializer_list<ID3D12CommandList*> lists) {
            std::vector<ID3D12CommandList*> array(lists);
            queue->ExecuteCommandLists(UINT(array.size()), array.data());
            if (rt::d3d12::last_execute_result(queue.p) != leonida::ReplayResult::success) {
                std::fprintf(stderr, "ExecuteCommandLists: replay result %d\n", int(rt::d3d12::last_execute_result(queue.p)));
                return false;
            }
            ++fence_value;
            return SUCCEEDED(queue->Signal(fence.p, fence_value)) &&
                   SUCCEEDED(fence->SetEventOnCompletion(fence_value, nullptr));
        };
        const auto new_buffer = [&](const D3D12_HEAP_PROPERTIES& heap, UINT64 bytes, D3D12_RESOURCE_STATES state,
                                    Ref<ID3D12Resource>& out, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
            const auto desc = buffer_desc(bytes, flags);
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                             IID_PPV_ARGS(&out.p)));
        };
        const auto make_root = [&](UINT count, const D3D12_ROOT_PARAMETER* params, Ref<ID3D12RootSignature>& out) {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
            root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
            root_desc.Desc_1_0 = {count, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
            const auto blob = serialize_root_signature(root_desc);
            return !blob.empty() &&
                   SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&out.p)));
        };
        const auto new_list = [&](Ref<ID3D12CommandAllocator>& allocator, Ref<ID3D12GraphicsCommandList>& list,
                                  ID3D12PipelineState* pso) {
            return SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))) &&
                   SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso,
                                                       IID_PPV_ARGS(&list.p)));
        };
        // Fills `bytes` of a default-heap buffer (created in COPY_DEST) with `data` through an upload buffer, in `list`.
        std::vector<ID3D12Resource*> uploads;  // released with the device's other children, after the last run
        struct ReleaseUploads {
            std::vector<ID3D12Resource*>& resources;
            ~ReleaseUploads() {
                for (ID3D12Resource* r : resources) r->Release();
            }
        } release_uploads{uploads};
        const auto fill = [&](ID3D12GraphicsCommandList* list, ID3D12Resource* buffer, const void* data, UINT64 bytes) {
            Ref<ID3D12Resource> upload;
            if (!new_buffer(upload_heap, bytes, D3D12_RESOURCE_STATE_GENERIC_READ, upload)) return false;
            void* mapped = nullptr;
            if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
            std::memcpy(mapped, data, bytes);
            upload->Unmap(0, nullptr);
            list->CopyBufferRegion(buffer, 0, upload.p, 0, bytes);
            uploads.push_back(upload.p);
            upload.p = nullptr;
            return true;
        };

        // ===== (a) draws =====
        {
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            param.Constants = {0, 0, 5};
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            Ref<ID3D12RootSignature> root;
            EXPECT(make_root(1, &param, root));
            const D3D12_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = root.p;
            desc.VS = {vs.data(), vs.size()};
            desc.PS = {ps.data(), ps.size()};
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = 0xffffffff;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable = TRUE;
            desc.InputLayout = {&element, 1};
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            Ref<ID3D12PipelineState> pso;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p))));

            const D3D12_INDIRECT_ARGUMENT_DESC arguments[] = {
                argument(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT, 0, 0, 5),
                argument(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW, 0),
                argument(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW),
                argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)};
            constexpr UINT draw_stride = 80;  // 20 + 16 + 16 + 20 bytes of arguments, padded
            const D3D12_COMMAND_SIGNATURE_DESC signature_desc{draw_stride, 4, arguments, 0};
            Ref<ID3D12CommandSignature> signature;
            EXPECT(SUCCEEDED(device->CreateCommandSignature(&signature_desc, root.p, IID_PPV_ARGS(&signature.p))));

            // Targets: a 64x64 RGBA8 image, its RTV, a readback buffer.
            const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                           D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            Ref<ID3D12DescriptorHeap> rtv_heap;
            EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
            using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv{SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p))};
            D3D12_RESOURCE_DESC image_desc{};
            image_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            image_desc.Width = image_desc.Height = target_size;
            image_desc.DepthOrArraySize = 1;
            image_desc.MipLevels = 1;
            image_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            image_desc.SampleDesc.Count = 1;
            image_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            Ref<ID3D12Resource> image, readback;
            EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &image_desc,
                                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                             IID_PPV_ARGS(&image.p))));
            device->CreateRenderTargetView(image.p, nullptr, rtv);
            constexpr UINT pitch = target_size * 4;  // 256: already aligned
            EXPECT(new_buffer(readback_heap, UINT64(pitch) * target_size, D3D12_RESOURCE_STATE_COPY_DEST, readback));

            // The vertex buffers (four quads each, eight bytes a vertex), the two index buffers, the records, the counts.
            std::vector<float> vertices[2];
            for (unsigned v = 0; v < 2; ++v)
                for (unsigned q = 0; q < 4; ++q) {
                    const float w = float(quad_size[v][q][0]) / 16.0f, h = float(quad_size[v][q][1]) / 16.0f;
                    for (const float p : {0.0f, 0.0f, w, 0.0f, 0.0f, h, w, h}) vertices[v].push_back(p);
                }
            std::vector<std::uint16_t> index16;
            std::vector<std::uint32_t> index32;
            for (unsigned q = 0; q < 4; ++q)
                for (const unsigned i : {0u, 1u, 2u, 2u, 1u, 3u}) {
                    index16.push_back(std::uint16_t(4 * q + i));
                    index32.push_back(4 * q + i);
                }
            Ref<ID3D12Resource> vertex_buffer[2], index_buffer[2], arguments_buffer, count_buffer, exact_buffer;
            const UINT64 vertex_bytes = vertices[0].size() * sizeof(float), index16_bytes = index16.size() * 2,
                         index32_bytes = index32.size() * 4;
            EXPECT(new_buffer(default_heap, vertex_bytes, D3D12_RESOURCE_STATE_COPY_DEST, vertex_buffer[0]) &&
                   new_buffer(default_heap, vertex_bytes, D3D12_RESOURCE_STATE_COPY_DEST, vertex_buffer[1]) &&
                   new_buffer(default_heap, index16_bytes, D3D12_RESOURCE_STATE_COPY_DEST, index_buffer[0]) &&
                   new_buffer(default_heap, index32_bytes, D3D12_RESOURCE_STATE_COPY_DEST, index_buffer[1]));
            constexpr UINT first_record = 256;  // the argument offset of the first call
            std::vector<std::uint8_t> records(first_record + 16 * draw_stride, 0xA5);  // bytes outside records are never read
            for (unsigned r = 0; r < 16; ++r) {
                const DrawRecord rec = draw_record(r);
                std::uint8_t* at = records.data() + first_record + r * draw_stride;
                const float constants[5] = {-1.0f + float(rec.cell % 4) * 0.5f, -1.0f + float(rec.cell / 4) * 0.5f,
                                            float(rec.r) / 255.0f, float(rec.g) / 255.0f, float(rec.b) / 255.0f};
                std::memcpy(at, constants, 20);
                const D3D12_VERTEX_BUFFER_VIEW vbv{vertex_buffer[rec.vertex_buffer]->GetGPUVirtualAddress() +
                                                       rec.vertex_offset * 8,
                                                   UINT(vertex_bytes - rec.vertex_offset * 8), 8};
                std::memcpy(at + 20, &vbv, 16);
                const bool wide = rec.index_buffer == 1;
                const D3D12_INDEX_BUFFER_VIEW ibv{index_buffer[rec.index_buffer]->GetGPUVirtualAddress(),
                                                  UINT(wide ? index32_bytes : index16_bytes),
                                                  wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
                std::memcpy(at + 36, &ibv, 16);
                const D3D12_DRAW_INDEXED_ARGUMENTS args{6, 1, 6 * rec.quad, -INT(rec.vertex_offset), 0};
                std::memcpy(at + 52, &args, 20);
            }
            const std::uint32_t counts[4] = {5, 0, 100, 0};
            EXPECT(new_buffer(default_heap, records.size(), D3D12_RESOURCE_STATE_COPY_DEST, arguments_buffer));
            EXPECT(new_buffer(default_heap, 256, D3D12_RESOURCE_STATE_COPY_DEST, count_buffer));
            // Records 0..7 again in a buffer that ends where the last record's arguments do (D3D12 asks for no more: the
            // 80 - 72 bytes of its stride past them need not exist).
            constexpr UINT exact_bytes = 7 * draw_stride + 72;
            EXPECT(new_buffer(default_heap, exact_bytes, D3D12_RESOURCE_STATE_COPY_DEST, exact_buffer));

            Ref<ID3D12CommandAllocator> allocator;
            Ref<ID3D12GraphicsCommandList> list;
            EXPECT(new_list(allocator, list, pso.p));
            EXPECT(fill(list.p, vertex_buffer[0].p, vertices[0].data(), vertex_bytes));
            EXPECT(fill(list.p, vertex_buffer[1].p, vertices[1].data(), vertex_bytes));
            EXPECT(fill(list.p, index_buffer[0].p, index16.data(), index16_bytes));
            EXPECT(fill(list.p, index_buffer[1].p, index32.data(), index32_bytes));
            EXPECT(fill(list.p, arguments_buffer.p, records.data(), records.size()));
            EXPECT(fill(list.p, exact_buffer.p, records.data() + first_record, exact_bytes));
            std::vector<std::uint8_t> count_bytes(256, 0);
            std::memcpy(count_bytes.data(), counts, sizeof(counts));
            EXPECT(fill(list.p, count_buffer.p, count_bytes.data(), 256));
            const D3D12_RESOURCE_BARRIER to_use[] = {
                transition(vertex_buffer[0].p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
                transition(vertex_buffer[1].p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
                transition(index_buffer[0].p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
                transition(index_buffer[1].p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
                transition(arguments_buffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
                transition(count_buffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
                transition(exact_buffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};
            list->ResourceBarrier(7, to_use);
            const FLOAT clear_colour[4] = {1.0f, 0.0f, 1.0f, 1.0f};
            list->ClearRenderTargetView(rtv, clear_colour, 0, nullptr);
            list->SetGraphicsRootSignature(root.p);
            const D3D12_VIEWPORT viewport{0, 0, float(target_size), float(target_size), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(target_size), LONG(target_size)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT64 at = first_record;
            list->ExecuteIndirect(signature.p, 8, arguments_buffer.p, at, nullptr, 0);                          // records 0..7
            list->ExecuteIndirect(signature.p, 8, arguments_buffer.p, at + 8 * draw_stride, count_buffer.p, 0);  // 5 of 8
            list->ExecuteIndirect(signature.p, 3, arguments_buffer.p, at + 13 * draw_stride, count_buffer.p, 4);  // count 0
            list->ExecuteIndirect(signature.p, 2, arguments_buffer.p, at + 14 * draw_stride, count_buffer.p, 8);  // 100 of 2
            list->ExecuteIndirect(signature.p, 8, exact_buffer.p, 0, nullptr, 0);                               // records 0..7 again
            const auto to_copy = transition(image.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_copy);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, target_size, target_size, 1, pitch}};
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = image.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            EXPECT(list->Close() == S_OK);
            EXPECT(run({list.p}));

            Image want;
            for (unsigned r = 0; r < 8; ++r) paint(want, draw_record(r));        // records 0..7
            for (unsigned r = 8; r < 13; ++r) paint(want, draw_record(r));       // 5 of 8
            for (unsigned r = 14; r < 16; ++r) paint(want, draw_record(r));      // count 100 of MaxCommandCount 2
            cells_drawn = 8 + 5 + 2;
            void* data = nullptr;
            EXPECT(SUCCEEDED(readback->Map(0, nullptr, &data)));
            draw_mismatches = compare("draw", static_cast<const std::uint8_t*>(data), pitch, want);
            readback->Unmap(0, nullptr);
        }

        // ===== (b) dispatches =====
        {
            D3D12_ROOT_PARAMETER params[2] = {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants = {0, 0, 4};
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[1].Descriptor = {0, 0};
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            Ref<ID3D12RootSignature> root;
            EXPECT(make_root(2, params, root));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
            pso_desc.pRootSignature = root.p;
            pso_desc.CS = {cs.data(), cs.size()};
            Ref<ID3D12PipelineState> pso;
            EXPECT(SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));
            const D3D12_INDIRECT_ARGUMENT_DESC arguments[] = {
                argument(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT, 0, 0, 4),
                argument(D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW, 1),
                argument(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)};
            constexpr UINT stride = 36;  // 16 + 8 + 12
            const D3D12_COMMAND_SIGNATURE_DESC signature_desc{stride, 3, arguments, 0};
            Ref<ID3D12CommandSignature> signature;
            EXPECT(SUCCEEDED(device->CreateCommandSignature(&signature_desc, root.p, IID_PPV_ARGS(&signature.p))));

            Ref<ID3D12Resource> output, readback, arguments_buffer, count_buffer;
            constexpr UINT regions = 8, output_bytes = regions * region_bytes;
            EXPECT(new_buffer(default_heap, output_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
            EXPECT(new_buffer(readback_heap, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST, readback));
            constexpr UINT first_record = 100;
            std::vector<std::uint8_t> records(first_record + regions * stride, 0xA5);
            for (unsigned r = 0; r < regions; ++r) {
                const DispatchRecord rec = dispatch_record(r);
                std::uint8_t* at = records.data() + first_record + r * stride;
                std::memcpy(at, &rec, 16);
                const D3D12_GPU_VIRTUAL_ADDRESS va = output->GetGPUVirtualAddress() + r * region_bytes;
                std::memcpy(at + 16, &va, 8);
                const D3D12_DISPATCH_ARGUMENTS args{rec.groups, 1, 1};
                std::memcpy(at + 24, &args, 12);
            }
            std::vector<std::uint8_t> count_bytes(256, 0);
            const std::uint32_t three = 3;
            std::memcpy(count_bytes.data() + 16, &three, 4);
            EXPECT(new_buffer(default_heap, records.size(), D3D12_RESOURCE_STATE_COPY_DEST, arguments_buffer));
            EXPECT(new_buffer(default_heap, 256, D3D12_RESOURCE_STATE_COPY_DEST, count_buffer));
            Ref<ID3D12CommandAllocator> allocator;
            Ref<ID3D12GraphicsCommandList> list;
            EXPECT(new_list(allocator, list, pso.p));
            EXPECT(fill(list.p, arguments_buffer.p, records.data(), records.size()));
            EXPECT(fill(list.p, count_buffer.p, count_bytes.data(), 256));
            const D3D12_RESOURCE_BARRIER to_use[] = {
                transition(arguments_buffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
                transition(count_buffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};
            list->ResourceBarrier(2, to_use);
            list->SetComputeRootSignature(root.p);
            list->ExecuteIndirect(signature.p, 4, arguments_buffer.p, first_record, nullptr, 0);                  // 0..3
            list->ExecuteIndirect(signature.p, 4, arguments_buffer.p, first_record + 4 * stride, count_buffer.p, 16);  // 3 of 4
            const auto to_copy = transition(output.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_copy);
            list->CopyBufferRegion(readback.p, 0, output.p, 0, output_bytes);
            EXPECT(list->Close() == S_OK);
            EXPECT(run({list.p}));

            void* data = nullptr;
            EXPECT(SUCCEEDED(readback->Map(0, nullptr, &data)));
            const auto* got = static_cast<const std::uint32_t*>(data);
            for (unsigned r = 0; r < regions; ++r) {
                const DispatchRecord rec = dispatch_record(r);
                const bool ran = r < 7;  // records 4..6 by the count of 3; record 7 is cut off
                if (ran) groups_run += rec.groups;
                for (unsigned i = 0; i < region_bytes / 4; ++i) {
                    const std::uint32_t want = (ran && i < 64 * rec.groups) ? (((i + rec.a) ^ rec.b) + rec.c) - rec.d : 0;
                    if (got[r * (region_bytes / 4) + i] == want) continue;
                    if (dispatch_mismatches++ < 6)
                        std::fprintf(stderr, "dispatch record %u dword %u: got %08x want %08x\n", r, i,
                                     got[r * (region_bytes / 4) + i], want);
                }
            }
            readback->Unmap(0, nullptr);
        }

        // ===== (c) CreateCommandSignature, against the host runtime =====
        {
            Ref<ID3D12Device> host;
            Ref<ID3D12RootSignature> host_root, layer_root;
            if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host.p)))) {
                Ref<IDXGIFactory4> factory;
                Ref<IDXGIAdapter1> adapter;
                if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory.p))) &&
                    SUCCEEDED(factory->EnumAdapterByLuid(host->GetAdapterLuid(), IID_PPV_ARGS(&adapter.p)))) {
                    DXGI_ADAPTER_DESC1 adapter_desc{};
                    adapter->GetDesc1(&adapter_desc);
                    host_name.clear();
                    for (const wchar_t* c = adapter_desc.Description; *c; ++c) host_name += char(*c);
                }
            }
            // [0] four constants, [1] a root CBV, [2] a table of one SRV, [3] a root SRV, [4] a root UAV.
            D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
            D3D12_ROOT_PARAMETER params[5] = {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants = {0, 0, 4};
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[1].Descriptor = {1, 0};
            params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[2].DescriptorTable = {1, &range};
            params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[3].Descriptor = {1, 0};
            params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[4].Descriptor = {0, 0};
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
            root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
            root_desc.Desc_1_0 = {5, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
            const auto blob = serialize_root_signature(root_desc);
            EXPECT(!blob.empty() && SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&layer_root.p))));
            if (host.p) EXPECT(SUCCEEDED(host->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&host_root.p))));

            using T = D3D12_INDIRECT_ARGUMENT_TYPE;
            const auto C = [](UINT index, UINT dest, UINT count) { return argument(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT, index, dest, count); };
            const auto A = [](T type, UINT index = 0) { return argument(type, index); };
            const std::vector<SignatureCase> signature_cases = {
                {"draw", {A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 16, false},
                {"draw with a root signature", {A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 16, true},
                {"draw, stride 64", {A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 64, false},
                {"constants + draw", {C(0, 0, 4), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 32, true},
                {"constants + draw, no root signature", {C(0, 0, 4), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 32, false},
                {"constants past the parameter", {C(0, 2, 4), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 32, true},
                {"constants, none set", {C(0, 0, 0), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 16, true},
                {"constants, no such parameter", {C(9, 0, 1), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 20, true},
                {"constants on a CBV parameter", {C(1, 0, 1), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 20, true},
                {"CBV + draw", {A(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW, 1), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 24, true},
                {"CBV on the constants parameter", {A(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW, 0), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 24, true},
                {"CBV on the table", {A(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW, 2), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 24, true},
                {"SRV + draw", {A(D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW, 3), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 24, true},
                {"SRV on the CBV parameter", {A(D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW, 1), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 24, true},
                {"UAV + dispatch", {A(D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW, 4), A(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)}, 20, true},
                {"constants + UAV + dispatch", {C(0, 0, 4), A(D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW, 4), A(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)}, 36, true},
                {"vertex buffer + index buffer + draw indexed", {A(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW, 0), A(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)}, 52, false},
                {"vertex buffer + draw indexed, no root", {A(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW, 31), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)}, 36, false},
                {"vertex buffer slot 32", {A(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW, 32), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 32, false},
                {"draw before constants", {A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW), C(0, 0, 1)}, 24, true},
                {"two draws", {A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 32, false},
                {"constants only", {C(0, 0, 1)}, 4, true},
                {"draw indexed, stride 16", {A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)}, 16, false},
                {"draw, stride 18", {A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 18, false},
                {"draw, stride 0", {A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 0, false},
                {"constants + dispatch", {C(0, 0, 4), A(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)}, 28, true},
                {"vertex buffer + dispatch", {A(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW, 0), A(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)}, 28, false},
                {"index buffer + dispatch", {A(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW), A(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)}, 28, false},
                {"vertex buffer slot twice", {A(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW, 1), A(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW, 1), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 48, false},
                {"two vertex buffers, two slots", {A(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW, 0), A(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW, 1), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 48, false},
                {"two index buffers", {A(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW), A(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)}, 52, false},
                {"index buffer + draw (not indexed)", {A(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW), A(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 32, false},
            };
            for (const SignatureCase& c : signature_cases) {
                D3D12_COMMAND_SIGNATURE_DESC desc{c.stride, UINT(c.arguments.size()), c.arguments.data(), 0};
                Ref<ID3D12CommandSignature> on_layer, on_host;
                const HRESULT layer_hr = device->CreateCommandSignature(&desc, c.root ? layer_root.p : nullptr,
                                                                        IID_PPV_ARGS(&on_layer.p));
                if (!host.p) {  // nothing to compare with: reported as skipped below, not as agreement
                    ++skipped_cases;
                    continue;
                }
                const HRESULT host_hr = host->CreateCommandSignature(&desc, c.root ? host_root.p : nullptr,
                                                                     IID_PPV_ARGS(&on_host.p));
                ++cases;
                if (SUCCEEDED(layer_hr) != SUCCEEDED(host_hr)) {
                    ++signature_mismatches;
                    std::fprintf(stderr, "CreateCommandSignature \"%s\": layer 0x%08lx, host 0x%08lx\n", c.name,
                                 static_cast<unsigned long>(layer_hr), static_cast<unsigned long>(host_hr));
                }
            }
        }
    }

    EXPECT(!draw_mismatches);
    EXPECT(!dispatch_mismatches);
    EXPECT(!signature_mismatches);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("multi-argument ExecuteIndirect: %u cells drawn by 5 calls (root constants, a vertex buffer view with a "
                "base vertex, a 16- or 32-bit index buffer view and DRAW_INDEXED per record; a count buffer: 5 of 8, "
                "0, 100 of 2; one call from an argument buffer exactly (n - 1) * stride + the arguments long) and %u "
                "groups run by 2 dispatch calls (root constants and a root UAV per record; 3 of 4 by a count buffer) "
                "matched the CPU reference (error 0)\n",
                cells_drawn, groups_run);
    if (skipped_cases)
        std::printf("CreateCommandSignature comparison SKIPPED: no host D3D12 runtime device (%u cases not compared)\n",
                    skipped_cases);
    else
        std::printf("%u CreateCommandSignature cases agree with the host runtime (%s)\n", cases, host_name.c_str());
    return 0;
}
