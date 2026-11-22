// Milestone M5 (docs/d3d12_vulkan.md section 6), the synthetic part without mesh shaders: the four things a world
// frame leans on, each through the D3D12 API on the proxy only (as the M2 and M3 tests do), with shaders that are
// translator output (hand-written RDNA2 in test/shaders: vs_m5, ps_m5_inst, vs_m3, ps_m5_flat, vs_m2, ps_m5_tex), and
// every pixel compared with a CPU reference (error 0).
//
// (a) ExecuteIndirect draws. Command signatures for DRAW (stride 16) and DRAW_INDEXED (strides 20 and 32), argument
//     records written by the CPU into an upload buffer, and by the GPU (CopyBufferRegion into default-heap buffers
//     placed at non-zero heap offsets, then a COPY_DEST -> INDIRECT_ARGUMENT barrier), argument offsets inside those
//     buffers, a count buffer (the draw count is the smaller of it and MaxCommandCount; a count of 0 draws nothing).
//     Each record draws one cell of an 8x8 grid (vs_m5.asm places a quad from the vertex index, so StartVertexLocation
//     or StartIndexLocation pick the cell; the colour says which cell and which variant).
// (b) Multi-threaded recording. Four std::threads each create their own allocator and list and record 40 draws of
//     random rectangles (root constants) at once; the four lists run in one ExecuteCommandLists between a clear list and
//     a readback list. The rectangles overlap across the lists, so the list order shows in the pixels. Eight rounds of
//     different seeds, each compared with the painter's-algorithm reference.
// (c) Bindless-table pressure. A shader-visible CBV/SRV/UAV heap of 5120 SRVs of 5120 distinct 2x2 textures, one
//     descriptor table of all of them. Each of the 5120 pixels of a 128x40 target is a draw (scissor rectangle of one
//     pixel) whose root constant is the byte offset of a slot (ps_m5_tex.asm s_loads the T# from table + offset); the
//     slots are visited in a scrambled order, so the last slot and every other one is used exactly once. The 5120 T#s
//     must all have distinct, non-zero entries in the descriptor hash table.
// (d) Pipeline cache. The same PSO created twice: the second creation must be answered by the device's VkPipelineCache
//     (VkPipelineCreationFeedback's application-cache-hit bit, counted by the layer) and add nothing to the cache's
//     data, and both PSOs draw identically.
// Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation (0 errors required).
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <thread>
#include <vector>

#ifndef LEONIDA_TEST_M5_DIR
#error "LEONIDA_TEST_M5_DIR must name the folder build.py wrote (spirv/index.json, fssm/*.fssm)"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slots 9/10: handle in RAX

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

using Pixel = std::array<std::uint8_t, 4>;
constexpr Pixel clear_pixel{255, 0, 255, 255};  // magenta: a pixel nothing drew shows

struct Image {
    UINT width = 0, height = 0;
    std::vector<Pixel> pixels;
    Image(UINT w, UINT h, Pixel fill = clear_pixel) : width(w), height(h), pixels(std::size_t(w) * h, fill) {}
    Pixel& at(UINT x, UINT y) { return pixels[std::size_t(y) * width + x]; }
};

// Compare a readback buffer (rows of `pitch` bytes) with the reference; the number of differing pixels.
unsigned compare(const char* what, const std::uint8_t* got, UINT pitch, Image& want) {
    unsigned mismatches = 0;
    for (UINT y = 0; y < want.height; ++y)
        for (UINT x = 0; x < want.width; ++x) {
            const std::uint8_t* g = got + std::size_t(y) * pitch + x * 4;
            const Pixel& w = want.at(x, y);
            if (std::memcmp(g, w.data(), 4) == 0) continue;
            if (mismatches++ < 6)
                std::fprintf(stderr, "%s pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", what, x, y, g[0], g[1],
                             g[2], g[3], w[0], w[1], w[2], w[3]);
        }
    return mismatches;
}

// vs_m5.asm / ps_m5_inst.asm: cell k (0..63) of the 8x8 grid on a 64x64 target, variant w.
void paint_cell(Image& image, unsigned k, unsigned w) {
    const Pixel colour{std::uint8_t(4 * k), std::uint8_t(4 * (63 - k)), std::uint8_t(128 * w), 255};
    for (UINT y = 0; y < 8; ++y)
        for (UINT x = 0; x < 8; ++x) image.at((k & 7) * 8 + x, (k >> 3) * 8 + y) = colour;
}

std::uint32_t next_random(std::uint32_t& state) {  // xorshift32
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

struct Rect {
    UINT x0, y0, x1, y1;
    std::uint8_t r, g, b;
};

// The eight root constants of vs_m3.asm / ps_m5_flat.asm for a rectangle in pixels of a 64x64 target (one pixel is
// 1/32 of the clip range, so every value is exact).
void rect_constants(const Rect& rect, float (&c)[8]) {
    c[0] = -1.0f + float(rect.x0) / 32.0f;
    c[1] = 1.0f - float(rect.y0) / 32.0f;
    c[2] = -1.0f + float(rect.x1) / 32.0f;
    c[3] = 1.0f - float(rect.y1) / 32.0f;
    c[4] = 0.0f;
    c[5] = float(rect.r) / 255.0f;
    c[6] = float(rect.g) / 255.0f;
    c[7] = float(rect.b) / 255.0f;
}

void paint_rect(Image& image, const Rect& rect) {
    for (UINT y = rect.y0; y < rect.y1; ++y)
        for (UINT x = rect.x0; x < rect.x1; ++x) image.at(x, y) = {rect.r, rect.g, rect.b, 255};
}

// What a worker thread of (b) draws: 40 rectangles from its seed.
std::vector<Rect> worker_rects(unsigned round, unsigned thread) {
    std::uint32_t state = 0x9e3779b9u ^ ((round * 8u + thread + 1u) * 2654435761u);
    std::vector<Rect> rects;
    for (unsigned i = 0; i < 40; ++i) {
        Rect r{};
        r.x0 = next_random(state) % 60;
        r.y0 = next_random(state) % 60;
        r.x1 = std::min<UINT>(64, r.x0 + 1 + next_random(state) % 24);
        r.y1 = std::min<UINT>(64, r.y0 + 1 + next_random(state) % 24);
        const std::uint32_t colour = next_random(state);
        r.r = std::uint8_t(colour);
        r.g = std::uint8_t(colour >> 8);
        r.b = std::uint8_t(colour >> 16);
        rects.push_back(r);
    }
    return rects;
}

// (c): the texel t (x + 2y) of texture i.
Pixel texel_colour(unsigned i, unsigned t) {
    return {std::uint8_t(i), std::uint8_t((i >> 8) * 40 + t * 50), std::uint8_t(i * 13 + t * 101), 255};
}
constexpr UINT texture_count = 5120;
constexpr UINT bindless_w = 128, bindless_h = 40;  // one pixel per texture
unsigned scrambled(unsigned pixel) { return (pixel * 2003u + 17u) % texture_count; }  // 2003 is coprime to 5120

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;   // no Vulkan GPU: ctest skips
    if (device_status != vktest::DeviceStatus::ready) return 1;
    if (!d.bindless) {
        std::printf("SKIP: the device lacks the descriptor indexing features of the bindless set 0\n");
        return 77;
    }
    // Without multiDrawIndirect the layer issues one call per record, which is still checked. Without
    // drawIndirectCount the calls that take a count buffer (P2, P4, P5) cannot run, and are left out.
    const bool counts = d.draw_indirect_count;
    if (!d.multi_draw_indirect)
        std::printf("note: no multiDrawIndirect: the layer issues one call per record\n");
    if (!counts)
        std::printf("note: no drawIndirectCount: the count-buffer calls (P2, P4, P5) are not run\n");

    const std::filesystem::path root_dir = LEONIDA_TEST_M5_DIR;
    leonida::ShaderLibrary library;
    EXPECT(library.load((root_dir / "spirv").string()));
    const auto container_of = [&](const char* name, const char* stage, std::vector<std::uint8_t>& out) {
        const auto program = read_file(root_dir / "fssm" / (std::string(name) + ".fssm"));
        if (program.size() < 4 || !library.find(stage, leonida::sha256_hex(program))) return false;
        out = shader_container(program);
        return true;
    };
    std::vector<std::uint8_t> vs_cells, ps_cells, vs_rect, ps_flat, vs_quad, ps_tex;
    EXPECT(container_of("vs_m5", "vs", vs_cells) && container_of("ps_m5_inst", "ps", ps_cells) &&
           container_of("vs_m3", "vs", vs_rect) && container_of("ps_m5_flat", "ps", ps_flat) &&
           container_of("vs_m2", "vs", vs_quad) && container_of("ps_m5_tex", "ps", ps_tex));

    // Everything the four parts report.
    unsigned indirect_mismatches = 0, thread_mismatches = 0, bindless_mismatches = 0, cache_mismatches = 0;
    unsigned cells_drawn = 0, thread_draws = 0, distinct_indices = 0;
    unsigned cache_first_created = 0, cache_second_created = 0, cache_second_hits = 0;
    std::size_t cache_bytes_first = 0, cache_bytes_second = 0;
    bool cache_available = false;
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
        // Run `lists` in one ExecuteCommandLists and wait for them.
        const auto run = [&](std::initializer_list<ID3D12CommandList*> lists) {
            std::vector<ID3D12CommandList*> array(lists);
            queue->ExecuteCommandLists(UINT(array.size()), array.data());
            if (rt::d3d12::last_execute_result(queue.p) != leonida::ReplayResult::success) return false;
            ++fence_value;
            return SUCCEEDED(queue->Signal(fence.p, fence_value)) &&
                   SUCCEEDED(fence->SetEventOnCompletion(fence_value, nullptr));
        };
        const auto new_buffer = [&](const D3D12_HEAP_PROPERTIES& heap, UINT64 bytes, D3D12_RESOURCE_STATES state,
                                    Ref<ID3D12Resource>& out) {
            const auto desc = buffer_desc(bytes);
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
        const auto pso_desc_of = [&](ID3D12RootSignature* root, const std::vector<std::uint8_t>& vs,
                                     const std::vector<std::uint8_t>& ps) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = root;
            desc.VS = {vs.data(), vs.size()};
            desc.PS = {ps.data(), ps.size()};
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = 0xffffffff;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable = TRUE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            return desc;
        };

        // Targets in COPY_SOURCE state, one RTV heap slot each, and a readback buffer for each.
        const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> rtv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        const std::uint64_t rtv_start = reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p);
        const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        struct Target {
            Ref<ID3D12Resource> image, readback;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            UINT width = 0, height = 0, pitch = 0;
        };
        const auto make_target = [&](Target& t, UINT slot_index, UINT w, UINT h) {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = w;
            desc.Height = h;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            t.width = w;
            t.height = h;
            t.pitch = (w * 4 + 255) & ~255u;
            if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                                       IID_PPV_ARGS(&t.image.p))))
                return false;
            t.rtv = {SIZE_T(rtv_start + std::uint64_t(slot_index) * rtv_step)};
            device->CreateRenderTargetView(t.image.p, nullptr, t.rtv);
            return new_buffer(readback_heap, std::uint64_t(t.pitch) * h, D3D12_RESOURCE_STATE_COPY_DEST, t.readback);
        };
        Target cells_target, thread_target, bindless_target, cache_target;
        EXPECT(make_target(cells_target, 0, 64, 64) && make_target(thread_target, 1, 64, 64) &&
               make_target(bindless_target, 2, bindless_w, bindless_h) && make_target(cache_target, 3, 64, 64));
        // COPY_SOURCE -> RENDER_TARGET and a clear; and RENDER_TARGET -> COPY_SOURCE and the copy to the readback.
        const FLOAT clear_colour[4] = {1.0f, 0.0f, 1.0f, 1.0f};
        const auto begin_target = [&](ID3D12GraphicsCommandList* list, Target& t) {
            const auto barrier = transition(t.image.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &barrier);
            list->ClearRenderTargetView(t.rtv, clear_colour, 0, nullptr);
        };
        const auto end_target = [&](ID3D12GraphicsCommandList* list, Target& t) {
            const auto barrier = transition(t.image.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = t.readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, t.width, t.height, 1, t.pitch}};
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = t.image.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        };
        const auto new_list = [&](Ref<ID3D12CommandAllocator>& allocator, Ref<ID3D12GraphicsCommandList>& list,
                                  ID3D12PipelineState* pso) {
            return SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))) &&
                   SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso,
                                                       IID_PPV_ARGS(&list.p)));
        };
        const auto set_target_state = [&](ID3D12GraphicsCommandList* list, Target& t) {
            const D3D12_VIEWPORT viewport{0, 0, float(t.width), float(t.height), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(t.width), LONG(t.height)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &t.rtv, FALSE, nullptr);
        };

        // ===== (a) ExecuteIndirect =====
        {
            Ref<ID3D12RootSignature> root;
            EXPECT(make_root(0, nullptr, root));
            auto desc = pso_desc_of(root.p, vs_cells, ps_cells);
            Ref<ID3D12PipelineState> pso;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p))));

            // The command signatures: strides 16 (draw), 20 and 32 (draw indexed; 32 leaves 12 bytes of padding).
            Ref<ID3D12CommandSignature> sig_draw, sig_idx20, sig_idx32;
            {
                D3D12_INDIRECT_ARGUMENT_DESC arg{};
                D3D12_COMMAND_SIGNATURE_DESC sig{};
                sig.NumArgumentDescs = 1;
                sig.pArgumentDescs = &arg;
                arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
                sig.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
                EXPECT(SUCCEEDED(device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&sig_draw.p))));
                arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
                sig.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
                EXPECT(SUCCEEDED(device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&sig_idx20.p))));
                sig.ByteStride = 32;
                EXPECT(SUCCEEDED(device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&sig_idx32.p))));
            }

            // The index buffer: for cell k and variant w, six indices 8k + 4w + {0 1 2 2 1 3} at slot 2k + w.
            // Upload buffer 1 (CPU-written draw records at 64 after the index buffer's 1536 bytes ... see below).
            Ref<ID3D12Resource> index_upload, args_cpu;
            EXPECT(new_buffer(upload_heap, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, index_upload));
            EXPECT(new_buffer(upload_heap, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, args_cpu));
            std::uint8_t* mapped = nullptr;
            EXPECT(SUCCEEDED(index_upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
            auto* indices = reinterpret_cast<std::uint16_t*>(mapped);
            for (unsigned k = 0; k < 64; ++k)
                for (unsigned w = 0; w < 2; ++w) {
                    const std::uint16_t b = std::uint16_t(8 * k + 4 * w), quad[6] = {
                        b, std::uint16_t(b + 1), std::uint16_t(b + 2), std::uint16_t(b + 2), std::uint16_t(b + 1),
                        std::uint16_t(b + 3)};
                    std::memcpy(indices + 6 * (2 * k + w), quad, sizeof(quad));
                }
            const D3D12_INDEX_BUFFER_VIEW ibv{index_upload->GetGPUVirtualAddress(), 128 * 6 * sizeof(std::uint16_t),
                                              DXGI_FORMAT_R16_UINT};

            // StartInstanceLocation: the shaders never read the instance index, so a non-zero start changes nothing
            // in the pixels; it is there to run the records the way a game's culling pass writes them (with
            // drawIndirectFirstInstance, which the device enables where it has it).
            const auto first_instance = [&](unsigned r) { return d.draw_indirect_first_instance ? r + 1 : 0u; };
            Image want(64, 64);
            // P1: CPU-written records in an upload buffer, argument offset 64, stride 16: strips of 4 vertices.
            // Record r draws cell r, variant 1 for r = 5 and 12, with no instances at all for r % 4 == 3, and twice
            // (InstanceCount 2) for r = 8.
            EXPECT(SUCCEEDED(args_cpu->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
            std::memset(mapped, 0xA5, 4096);  // bytes outside the records are never read
            for (unsigned r = 0; r < 16; ++r) {
                const unsigned w = (r == 5 || r == 12) ? 1 : 0;
                const D3D12_DRAW_ARGUMENTS record{4, (r % 4 == 3) ? 0u : (r == 8 ? 2u : 1u), 8 * r + 4 * w,
                                                  first_instance(r)};
                std::memcpy(mapped + 64 + r * sizeof(record), &record, sizeof(record));
                if (record.InstanceCount) { paint_cell(want, r, w); ++cells_drawn; }
            }
            // P2..P5 records and the count words, built in one upload buffer and copied into placed default buffers.
            //   args2 (placed at 64 KiB): 20 indexed records of stride 32 at 96 for cells 16..35 (variant r % 2, none
            //   at r = 6), then 4 records at 736 for cells 44..47 (never drawn: count 0), then 6 at 864 for cells
            //   48..53 (only the first two drawn: MaxCommandCount 2 against a count of 13).
            //   counts (placed at 128 KiB): 13 at 8, 0 at 12.
            //   args3 (placed at 192 KiB): 8 records of stride 20 at 12 for cells 36..43 (variant (r + 1) % 2).
            Ref<ID3D12Resource> gpu_upload;
            EXPECT(new_buffer(upload_heap, 8192, D3D12_RESOURCE_STATE_GENERIC_READ, gpu_upload));
            std::uint8_t* up = nullptr;
            EXPECT(SUCCEEDED(gpu_upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
            std::memset(up, 0xA5, 8192);
            const auto put_indexed = [&](std::uint8_t* at, unsigned cell, unsigned w, unsigned instances) {
                const D3D12_DRAW_INDEXED_ARGUMENTS record{6, instances, 6 * (2 * cell + w), 0, first_instance(cell)};
                std::memcpy(at, &record, sizeof(record));
            };
            for (unsigned r = 0; r < 20; ++r) {
                put_indexed(up + 96 + r * 32, 16 + r, r % 2, r == 6 ? 0 : 1);
                if (counts && r < 13 && r != 6) { paint_cell(want, 16 + r, r % 2); ++cells_drawn; }
            }
            for (unsigned r = 0; r < 4; ++r) put_indexed(up + 736 + r * 32, 44 + r, 0, 1);
            for (unsigned r = 0; r < 6; ++r) put_indexed(up + 864 + r * 32, 48 + r, 1, 1);
            if (counts) {
                paint_cell(want, 48, 1);
                paint_cell(want, 49, 1);
                cells_drawn += 2;
            }
            std::uint32_t* counts = reinterpret_cast<std::uint32_t*>(up + 4096);
            counts[2] = 13;
            counts[3] = 0;
            for (unsigned r = 0; r < 8; ++r) {
                put_indexed(up + 4096 + 256 + 12 + r * 20, 36 + r, (r + 1) % 2, 1);
                paint_cell(want, 36 + r, (r + 1) % 2);
                ++cells_drawn;
            }

            const D3D12_HEAP_DESC heap_desc{256 * 1024, {D3D12_HEAP_TYPE_DEFAULT}, 64 * 1024,
                                            D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
            Ref<ID3D12Heap> heap;
            EXPECT(SUCCEEDED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
            Ref<ID3D12Resource> args2, count_buffer, args3;
            const auto placed = [&](UINT64 offset, UINT64 bytes, Ref<ID3D12Resource>& out) {
                const auto desc2 = buffer_desc(bytes);
                return SUCCEEDED(device->CreatePlacedResource(heap.p, offset, &desc2, D3D12_RESOURCE_STATE_COPY_DEST,
                                                              nullptr, IID_PPV_ARGS(&out.p)));
            };
            EXPECT(placed(64 * 1024, 4096, args2) && placed(128 * 1024, 256, count_buffer) &&
                   placed(192 * 1024, 4096, args3));

            Ref<ID3D12CommandAllocator> allocator;
            Ref<ID3D12GraphicsCommandList> list;
            EXPECT(new_list(allocator, list, pso.p));
            list->CopyBufferRegion(args2.p, 0, gpu_upload.p, 0, 2048);
            list->CopyBufferRegion(count_buffer.p, 0, gpu_upload.p, 4096, 256);
            list->CopyBufferRegion(args3.p, 0, gpu_upload.p, 4096 + 256, 256);
            const D3D12_RESOURCE_BARRIER to_args[3] = {
                transition(args2.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
                transition(count_buffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
                transition(args3.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};
            list->ResourceBarrier(3, to_args);
            begin_target(list.p, cells_target);
            list->SetGraphicsRootSignature(root.p);
            set_target_state(list.p, cells_target);
            list->IASetIndexBuffer(&ibv);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            list->ExecuteIndirect(sig_draw.p, 16, args_cpu.p, 64, nullptr, 0);                 // P1
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (counts) list->ExecuteIndirect(sig_idx32.p, 20, args2.p, 96, count_buffer.p, 8);  // P2: count 13 of 20
            list->ExecuteIndirect(sig_idx20.p, 8, args3.p, 12, nullptr, 0);                      // P3: no count buffer
            if (counts) list->ExecuteIndirect(sig_idx32.p, 4, args2.p, 736, count_buffer.p, 12);  // P4: count 0
            if (counts) list->ExecuteIndirect(sig_idx32.p, 2, args2.p, 864, count_buffer.p, 8);  // P5: min(2, 13)
            end_target(list.p, cells_target);
            EXPECT(list->Close() == S_OK);
            EXPECT(run({list.p}));
            void* data = nullptr;
            EXPECT(SUCCEEDED(cells_target.readback->Map(0, nullptr, &data)));
            indirect_mismatches = compare("indirect", static_cast<const std::uint8_t*>(data), cells_target.pitch, want);
        }

        // ===== (b) multi-threaded recording =====
        {
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            param.Constants = {0, 0, 8};
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            Ref<ID3D12RootSignature> root;
            EXPECT(make_root(1, &param, root));
            const auto desc = pso_desc_of(root.p, vs_rect, ps_flat);
            Ref<ID3D12PipelineState> pso;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p))));

            constexpr unsigned threads = 4, rounds = 8;
            std::vector<std::uint8_t> first_round_bytes;
            for (unsigned round = 0; round < rounds; ++round) {
                Ref<ID3D12CommandAllocator> allocators[threads];
                Ref<ID3D12GraphicsCommandList> lists[threads];
                std::atomic<unsigned> ready{0};
                std::atomic<bool> go{false};
                std::atomic<unsigned> failures{0};
                std::vector<std::thread> pool;
                for (unsigned t = 0; t < threads; ++t)
                    pool.emplace_back([&, t] {
                        ready.fetch_add(1);
                        while (!go.load()) std::this_thread::yield();  // all four start recording together
                        if (!new_list(allocators[t], lists[t], pso.p)) { failures.fetch_add(1); return; }
                        ID3D12GraphicsCommandList* l = lists[t].p;
                        l->SetGraphicsRootSignature(root.p);
                        set_target_state(l, thread_target);
                        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                        for (const Rect& rect : worker_rects(round, t)) {
                            float constants[8];
                            rect_constants(rect, constants);
                            l->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
                            l->DrawInstanced(4, 1, 0, 0);
                        }
                        if (l->Close() != S_OK) failures.fetch_add(1);
                    });
                while (ready.load() < threads) std::this_thread::yield();
                go.store(true);
                for (auto& thread : pool) thread.join();
                EXPECT(!failures.load());

                Ref<ID3D12CommandAllocator> first_allocator, last_allocator;
                Ref<ID3D12GraphicsCommandList> first, last;
                EXPECT(new_list(first_allocator, first, nullptr) && new_list(last_allocator, last, nullptr));
                begin_target(first.p, thread_target);
                EXPECT(first->Close() == S_OK);
                end_target(last.p, thread_target);
                EXPECT(last->Close() == S_OK);
                // The clear list, the four workers (in thread order) and the readback list in one call.
                EXPECT(run({first.p, lists[0].p, lists[1].p, lists[2].p, lists[3].p, last.p}));
                Image want(64, 64);
                for (unsigned t = 0; t < threads; ++t)
                    for (const Rect& rect : worker_rects(round, t)) { paint_rect(want, rect); ++thread_draws; }
                void* data = nullptr;
                EXPECT(SUCCEEDED(thread_target.readback->Map(0, nullptr, &data)));
                thread_mismatches += compare("threads", static_cast<const std::uint8_t*>(data), thread_target.pitch, want);
            }
        }

        // ===== (c) bindless-table pressure =====
        {
            D3D12_DESCRIPTOR_RANGE srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, texture_count, 0, 0, 0};
            D3D12_DESCRIPTOR_RANGE sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
            D3D12_ROOT_PARAMETER params[3] = {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[0].DescriptorTable = {1, &srv_range};
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable = {1, &sampler_range};
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[2].Constants = {0, 0, 1};
            params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            Ref<ID3D12RootSignature> root;
            EXPECT(make_root(3, params, root));
            const auto desc = pso_desc_of(root.p, vs_quad, ps_tex);
            Ref<ID3D12PipelineState> pso;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p))));

            const D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, texture_count,
                                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            const D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1,
                                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            Ref<ID3D12DescriptorHeap> srv_heap, sampler_heap;
            EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap.p))));
            EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&sampler_heap_desc, IID_PPV_ARGS(&sampler_heap.p))));
            const std::uint64_t srv_cpu = reinterpret_cast<XboxHeapHandleFn>(slot(srv_heap.p, 9))(srv_heap.p);
            const std::uint64_t srv_gpu = reinterpret_cast<XboxHeapHandleFn>(slot(srv_heap.p, 10))(srv_heap.p);
            const std::uint64_t sampler_cpu = reinterpret_cast<XboxHeapHandleFn>(slot(sampler_heap.p, 9))(sampler_heap.p);
            const std::uint64_t sampler_gpu = reinterpret_cast<XboxHeapHandleFn>(slot(sampler_heap.p, 10))(sampler_heap.p);
            const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            EXPECT(srv_cpu && sampler_cpu && srv_gpu >= 0x10000 && sampler_gpu >= 0x10000 && step >= 32);
            EXPECT(srv_gpu < (1ull << 32) && sampler_gpu < (1ull << 32));

            D3D12_SAMPLER_DESC sampler_desc{};
            sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler_desc.MaxAnisotropy = 1;
            sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
            device->CreateSampler(&sampler_desc, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(sampler_cpu)});

            // 5120 textures of 2x2, 512 bytes apiece in the upload buffer (rows at the 256-byte pitch).
            Ref<ID3D12Resource> upload;
            EXPECT(new_buffer(upload_heap, std::uint64_t(texture_count) * 512, D3D12_RESOURCE_STATE_GENERIC_READ, upload));
            std::uint8_t* up = nullptr;
            EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
            D3D12_RESOURCE_DESC texture_desc{};
            texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texture_desc.Width = texture_desc.Height = 2;
            texture_desc.DepthOrArraySize = 1;
            texture_desc.MipLevels = 1;
            texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texture_desc.SampleDesc.Count = 1;
            std::vector<Ref<ID3D12Resource>> textures(texture_count);
            for (UINT i = 0; i < texture_count; ++i) {
                for (unsigned t = 0; t < 4; ++t) {
                    const Pixel c = texel_colour(i, t);
                    std::memcpy(up + std::size_t(i) * 512 + (t / 2) * 256 + (t % 2) * 4, c.data(), 4);
                }
                if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&textures[i].p)))) {
                    std::fprintf(stderr, "texture %u of %u could not be created\n", i, texture_count);
                    return 1;
                }
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D = {0, 1, 0, 0.0f};
                device->CreateShaderResourceView(textures[i].p, &srv,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(srv_cpu + std::uint64_t(i) * step)});
            }
            // The hash table: every T# has an entry and no two share an index.
            std::set<std::uint32_t> seen;
            for (UINT i = 0; i < texture_count; ++i) {
                std::uint32_t key[8];
                std::memcpy(key, reinterpret_cast<const void*>(srv_cpu + std::uint64_t(i) * step), 32);
                const std::uint32_t index = rt::d3d12::bindless_hash_lookup(device.p, key);
                if (index) seen.insert(index);
            }
            distinct_indices = unsigned(seen.size());
            std::fprintf(stderr, "hash table: %u of %u T#s have distinct non-zero bindless indices\n", distinct_indices,
                         texture_count);
            EXPECT(distinct_indices == texture_count);

            Ref<ID3D12CommandAllocator> allocator;
            Ref<ID3D12GraphicsCommandList> list;
            EXPECT(new_list(allocator, list, pso.p));
            for (UINT i = 0; i < texture_count; ++i) {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = textures[i].p;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = upload.p;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = {std::uint64_t(i) * 512, {DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256}};
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            std::vector<D3D12_RESOURCE_BARRIER> barriers;
            for (UINT i = 0; i < texture_count; ++i)
                barriers.push_back(transition(textures[i].p, D3D12_RESOURCE_STATE_COPY_DEST,
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
            begin_target(list.p, bindless_target);
            list->SetGraphicsRootSignature(root.p);
            ID3D12DescriptorHeap* heaps[] = {srv_heap.p, sampler_heap.p};
            list->SetDescriptorHeaps(2, heaps);
            set_target_state(list.p, bindless_target);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            Ref<ID3D12Resource> index_upload;
            EXPECT(new_buffer(upload_heap, 256, D3D12_RESOURCE_STATE_GENERIC_READ, index_upload));
            std::uint8_t* index_data = nullptr;
            EXPECT(SUCCEEDED(index_upload->Map(0, nullptr, reinterpret_cast<void**>(&index_data))));
            const std::uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
            std::memcpy(index_data, quad, sizeof(quad));
            const D3D12_INDEX_BUFFER_VIEW ibv{index_upload->GetGPUVirtualAddress(), sizeof(quad), DXGI_FORMAT_R16_UINT};
            list->IASetIndexBuffer(&ibv);
            list->SetGraphicsRootDescriptorTable(0, D3D12_GPU_DESCRIPTOR_HANDLE{srv_gpu});
            list->SetGraphicsRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{sampler_gpu});
            Image want(bindless_w, bindless_h);
            for (UINT p = 0; p < bindless_w * bindless_h; ++p) {
                const UINT x = p % bindless_w, y = p / bindless_w, texture = scrambled(p);
                const D3D12_RECT scissor{LONG(x), LONG(y), LONG(x + 1), LONG(y + 1)};
                list->RSSetScissorRects(1, &scissor);
                const UINT offset = texture * step;
                list->SetGraphicsRoot32BitConstants(2, 1, &offset, 0);
                list->DrawIndexedInstanced(6, 1, 0, 0, 0);
                const unsigned texel = (x >= bindless_w / 2 ? 1u : 0u) + (y >= bindless_h / 2 ? 2u : 0u);
                want.at(x, y) = texel_colour(texture, texel);
            }
            bool last_used = false;
            for (UINT p = 0; p < texture_count; ++p) last_used |= scrambled(p) == texture_count - 1;
            EXPECT(last_used);
            end_target(list.p, bindless_target);
            EXPECT(list->Close() == S_OK);
            EXPECT(run({list.p}));
            void* data = nullptr;
            EXPECT(SUCCEEDED(bindless_target.readback->Map(0, nullptr, &data)));
            bindless_mismatches = compare("bindless", static_cast<const std::uint8_t*>(data), bindless_target.pitch, want);
        }

        // ===== (d) pipeline cache =====
        {
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            param.Constants = {0, 0, 8};
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            Ref<ID3D12RootSignature> root;
            EXPECT(make_root(1, &param, root));
            auto desc = pso_desc_of(root.p, vs_rect, ps_flat);
            // A state nothing above used, so the first creation has nothing to be answered from: no alpha written
            // (the target is cleared to alpha 1 and the shader writes 1, so the pixels do not change).
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0x7;
            const auto before = rt::d3d12::pipeline_cache_stats(device.p);
            Ref<ID3D12PipelineState> first_pso, second_pso;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&first_pso.p))));
            const auto after_first = rt::d3d12::pipeline_cache_stats(device.p);
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&second_pso.p))));
            const auto after_second = rt::d3d12::pipeline_cache_stats(device.p);
            cache_available = after_second.available;
            cache_first_created = after_first.created - before.created;
            cache_second_created = after_second.created - after_first.created;
            cache_second_hits = after_second.hits - after_first.hits;
            cache_bytes_first = after_first.bytes;
            cache_bytes_second = after_second.bytes;
            std::fprintf(stderr, "pipeline cache: available %d; first creation %u pipeline(s), %u hit(s); second %u, %u "
                                 "hit(s); cache data %zu -> %zu bytes (before: %zu)\n",
                         int(cache_available), cache_first_created, after_first.hits - before.hits, cache_second_created,
                         cache_second_hits, cache_bytes_first, cache_bytes_second, before.bytes);
            EXPECT(first_pso.p != second_pso.p);

            // Both PSOs draw three overlapping rectangles, and the pixels are what the CPU says.
            const Rect scene[3] = {{4, 4, 40, 30, 200, 30, 90}, {20, 16, 60, 58, 10, 220, 120}, {30, 2, 34, 62, 255, 255, 0}};
            Image want(64, 64);
            for (const Rect& rect : scene) paint_rect(want, rect);
            std::vector<std::uint8_t> drawn[2];
            ID3D12PipelineState* psos[2] = {first_pso.p, second_pso.p};
            for (unsigned which = 0; which < 2; ++which) {
                Ref<ID3D12CommandAllocator> allocator;
                Ref<ID3D12GraphicsCommandList> list;
                EXPECT(new_list(allocator, list, psos[which]));
                begin_target(list.p, cache_target);
                list->SetGraphicsRootSignature(root.p);
                set_target_state(list.p, cache_target);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                for (const Rect& rect : scene) {
                    float constants[8];
                    rect_constants(rect, constants);
                    list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
                    list->DrawInstanced(4, 1, 0, 0);
                }
                end_target(list.p, cache_target);
                EXPECT(list->Close() == S_OK);
                EXPECT(run({list.p}));
                void* data = nullptr;
                EXPECT(SUCCEEDED(cache_target.readback->Map(0, nullptr, &data)));
                cache_mismatches += compare(which ? "cache (second PSO)" : "cache (first PSO)",
                                            static_cast<const std::uint8_t*>(data), cache_target.pitch, want);
                drawn[which].assign(static_cast<const std::uint8_t*>(data),
                                    static_cast<const std::uint8_t*>(data) + std::size_t(cache_target.pitch) * 64);
            }
            EXPECT(drawn[0] == drawn[1]);
        }

        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) >= 1);
    }

    EXPECT(!indirect_mismatches);
    EXPECT(!thread_mismatches);
    EXPECT(!bindless_mismatches);
    EXPECT(!cache_mismatches);
    // The counters only move when the driver returns valid VkPipelineCreationFeedback (software ICDs may not); then
    // only the pixels, compared above, are checked.
    const bool feedback_valid = cache_available && cache_first_created >= 1 && cache_second_created >= 1;
    if (feedback_valid) {
        EXPECT(cache_second_created == cache_first_created);
        EXPECT(!cache_second_hits || cache_second_hits == cache_second_created);
        if (!cache_second_hits)  // a driver that never sets the hit bit still shows a miss as growth of the cache
            std::printf("note: the driver reported no application-cache hit for the second creation\n");
        if (cache_bytes_first && cache_bytes_second) {
            EXPECT(cache_bytes_second == cache_bytes_first);  // the second creation added nothing to the cache
        }
    } else {
        std::printf("note: pipeline cache feedback unavailable (no Vulkan 1.3 feedback, no valid bits or no cache): "
                    "only the pixels were checked\n");
    }
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("M5 (without mesh shaders): %u cells drawn by 5 ExecuteIndirect calls (draw and draw-indexed signatures, "
                "CPU- and GPU-written arguments at offsets in placed buffers, count buffer, count 0 and count above "
                "MaxCommandCount), %u rectangles recorded on 4 threads x 8 rounds, %u pixels each drawn from its own "
                "texture out of %u SRVs (%u distinct hash-table indices), a PSO created twice (%u of %u creations "
                "answered by the pipeline cache): every pixel matched the CPU reference (error 0)\n",
                cells_drawn, thread_draws, bindless_w * bindless_h, texture_count, distinct_indices, cache_second_hits,
                cache_second_created);
    return 0;
}
