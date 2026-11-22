// ID3D12GraphicsCommandList1::SetSamplePositions on VK_EXT_sample_locations, through the Xbox-ABI proxy. Every
// check draws one red triangle into a blue 64x64 multisampled target, then resolves it and reads it back. The
// triangle has the top-left corner and its edge runs from the top-right corner to (0, 47.5). With that slope and
// the half-pixel end, the edge crosses pixels at many offsets, fine enough to tell a pattern from its mirror image
// (the 4x standard pattern transposed is its mirror; an edge through whole-pixel corners can't tell them apart).
// A pixel then holds the share of its samples the triangle covers:
//   - the standard tables: for 2, 4 and 8 samples, a D3D12 device whose pipelines set the positions dynamically
//     (the standard ones when the list sets none) gives exactly the image of a D3D12 device on the same GPU whose
//     pipelines leave them to the driver;
//   - custom positions for every pixel, and a 2x2 pattern (four pixels' own positions): every resolved pixel is the
//     share of its samples on the triangle's side of the edge, computed here from the positions;
//   - SetSamplePositions(0, 0, nullptr) returns to the standard image;
//   - invalid calls (two pixels, a coordinate of 8) make Close return E_INVALIDARG, and positions for 2 samples on
//     a 4x PSO fail the list at execute.
// Skipped where the GPU lacks VK_EXT_sample_locations. Shaders: test/glsl/vs_draw.vert, ps_draw.frag. No game data.
// Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>

#if !defined(LEONIDA_TEST_VS_SPV) || !defined(LEONIDA_TEST_PS_SPV)
#error "LEONIDA_TEST_VS_SPV and LEONIDA_TEST_PS_SPV must name the compiled test shaders"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)
#define CHECK(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return false; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

namespace {

constexpr UINT kSize = 64;

struct Vertex {
    float x, y;
    std::uint8_t r, g, b, a;
};

struct Shaders {
    std::vector<std::uint8_t> vs, ps;
    std::filesystem::path library;
};

using Positions = std::function<void(ID3D12GraphicsCommandList1*)>;

// One draw of the triangle at `samples`, resolved into `pixels` (RGBA8, 256-byte rows). `positions` runs on the
// list before the draw. `expect` is what ExecuteCommandLists must report; pixels are read only on success.
bool render(vktest::Device& d, const Shaders& shaders, VkSampleCountFlags sample_locations, UINT samples,
            const Positions& positions, std::vector<std::uint8_t>& pixels,
            leonida::ReplayResult expect = leonida::ReplayResult::success, HRESULT close = S_OK, UINT quality = 0) {
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.buffer_device_address = true;
    info.sample_locations = sample_locations;
    info.shader_dir = shaders.library.string();
    ID3D12Device* inner = nullptr;
    CHECK(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
    Ref<ID3D12Device> device;
    CHECK(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                      reinterpret_cast<void**>(&device.p))));
    rt::d3d12::set_device_outer(inner, device.p);
    {
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor = {1, 0};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        const auto root_blob = serialize_root_signature(root_desc);
        CHECK(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        CHECK(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

        const D3D12_INPUT_ELEMENT_DESC elements[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.VS = {shaders.vs.data(), shaders.vs.size()};
        pso_desc.PS = {shaders.ps.data(), shaders.ps.size()};
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = 0xffffffff;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.InputLayout = {elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc = {samples, quality};
        Ref<ID3D12PipelineState> pso;
        CHECK(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

        const Vertex vertices[3] = {{-1, 1, 255, 0, 0, 255}, {1, 1, 255, 0, 0, 255}, {-1, -0.484375f, 255, 0, 0, 255}};
        const float tint[3] = {1, 0, 1};
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> upload, readback, target, resolved;
        const auto upload_desc = buffer_desc(4096);
        CHECK(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&upload.p))));
        const auto readback_desc = buffer_desc(kSize * 256);
        CHECK(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&readback.p))));
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = kSize;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc = {samples, 0};
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        CHECK(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                        IID_PPV_ARGS(&target.p))));
        D3D12_RESOURCE_DESC single_desc = rt_desc;
        single_desc.SampleDesc = {1, 0};
        CHECK(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &single_desc,
                                                        D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                        IID_PPV_ARGS(&resolved.p))));
        std::uint8_t* up = nullptr;
        CHECK(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
        std::memcpy(up, vertices, sizeof(vertices));
        std::memcpy(up + 512, tint, sizeof(tint));
        const D3D12_GPU_VIRTUAL_ADDRESS upload_va = upload->GetGPUVirtualAddress();

        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> rtv_heap;
        CHECK(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p))};
        device->CreateRenderTargetView(target.p, nullptr, rtv);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12GraphicsCommandList1> list1;
        Ref<ID3D12Fence> fence;
        CHECK(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso.p,
                                                  IID_PPV_ARGS(&list.p))));
        CHECK(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list1.p))));
        CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        const FLOAT blue[4] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        list->SetGraphicsRootSignature(root.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{upload_va, sizeof(vertices), sizeof(Vertex)};
        list->IASetVertexBuffers(0, 1, &vbv);
        const D3D12_VIEWPORT viewport{0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float origin[2] = {0, 0};
        list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
        if (positions) positions(list1.p);
        list->DrawInstanced(3, 1, 0, 0);
        if (samples > 1) {
            D3D12_RESOURCE_BARRIER to_resolve = transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                           D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            list->ResourceBarrier(1, &to_resolve);
            list->ResolveSubresource(resolved.p, 0, target.p, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
            D3D12_RESOURCE_BARRIER to_copy = transition(resolved.p, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                                        D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_copy);
        } else {  // single-sampled: the target is the image
            D3D12_RESOURCE_BARRIER to_copy = transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                        D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_copy);
        }
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, 256}};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = samples > 1 ? resolved.p : target.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        CHECK(list->Close() == close);
        if (close == S_OK) {
            ID3D12CommandList* lists[] = {list.p};
            queue->ExecuteCommandLists(1, lists);
            CHECK(rt::d3d12::last_execute_result(queue.p) == expect);
            CHECK(SUCCEEDED(queue->Signal(fence.p, 1)));
            CHECK(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
            if (expect == leonida::ReplayResult::success) {
                void* mapped = nullptr;
                CHECK(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
                pixels.assign(static_cast<const std::uint8_t*>(mapped),
                              static_cast<const std::uint8_t*>(mapped) + kSize * 256);
            }
        }
    }
    CHECK(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    return true;
}

// Every pixel against the share of its samples the triangle covers. In pixel units the triangle is
// 47.5 x + 64 y < 64 * 47.5, so 95 x + 128 y < 6080; a sample at D3D12 position (X, Y) of pixel (col, row) sits at
// col + 0.5 + X/16, so in 1/16 pixel it is covered when 95 (16 col + 8 + X) + 128 (16 row + 8 + Y) < 97280. A
// sample exactly on the edge is left to the fill rule, so such pixels are skipped. `grid` is 1 or 2 (the 2x2
// pattern repeats from the origin).
unsigned compare_coverage(const std::vector<std::uint8_t>& pixels, UINT samples, UINT grid,
                          const std::vector<D3D12_SAMPLE_POSITION>& positions, unsigned& partial) {
    unsigned mismatches = 0;
    partial = 0;
    for (UINT row = 0; row < kSize; ++row) {
        for (UINT col = 0; col < kSize; ++col) {
            const UINT pixel = (row % grid) * grid + (col % grid);
            int covered = 0;
            bool on_edge = false;
            for (UINT s = 0; s < samples; ++s) {
                const D3D12_SAMPLE_POSITION& p = positions[pixel * samples + s];
                const int at = 95 * (16 * int(col) + 8 + p.X) + 128 * (16 * int(row) + 8 + p.Y);
                on_edge |= at == 97280;
                covered += at < 97280;
            }
            if (on_edge) continue;
            const std::uint8_t* got = pixels.data() + row * 256 + col * 4;
            const float red = 255.0f * covered / samples;
            if (covered && covered < int(samples)) ++partial;
            if (std::abs(got[0] - red) > 1.01f || std::abs(got[2] - (255.0f - red)) > 1.01f || got[1] || got[3] != 255) {
                if (mismatches++ < 6)
                    std::fprintf(stderr, "pixel (%u, %u): got %u,%u,%u,%u want red %.2f of %u samples\n", col, row,
                                 got[0], got[1], got[2], got[3], red, samples);
            }
        }
    }
    return mismatches;
}

}  // namespace

int main() {
    vktest::Device d;
    EXPECT(d.create());
    if (!(d.sample_locations & VK_SAMPLE_COUNT_4_BIT)) {
        std::printf("SKIP: no VK_EXT_sample_locations with 4 samples on this GPU\n");
        return 77;  // ctest's SKIP_RETURN_CODE
    }

    Shaders shaders;
    const auto vs_program = fssm_program({0x56530001, 0xbf810000});
    const auto ps_program = fssm_program({0x50530001, 0xbf810000});
    shaders.vs = shader_container(vs_program);
    shaders.ps = shader_container(ps_program);
    shaders.library = std::filesystem::temp_directory_path() / "leonida_sample_positions_test";
    std::filesystem::create_directories(shaders.library);
    std::filesystem::copy_file(LEONIDA_TEST_VS_SPV, shaders.library / "vs_draw.spv",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_PS_SPV, shaders.library / "ps_draw.spv",
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream index(shaders.library / "index.json", std::ios::binary);
        index << "{\"vs_draw\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": ["
                 "{\"semantic\": \"COLOR\", \"semantic_index\": 0, \"register\": 0, \"mask\": 15, \"rw_mask\": 15, "
                 "\"system_value\": 0, \"component_type\": \"float\"}, "
                 "{\"semantic\": \"POSITION\", \"semantic_index\": 0, \"register\": 1, \"mask\": 3, \"rw_mask\": 3, "
                 "\"system_value\": 0, \"component_type\": \"float\"}]},\n"
                 " \"ps_draw\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }

    // 1. The standard tables: dynamic standard positions against the driver's own, per count.
    unsigned counts = 0;
    for (UINT samples : {2u, 4u, 8u}) {
        if (!(d.sample_locations & samples)) continue;
        std::vector<std::uint8_t> driver, dynamic;
        EXPECT(render(d, shaders, 0, samples, nullptr, driver));
        EXPECT(render(d, shaders, d.sample_locations, samples, nullptr, dynamic));
        if (driver != dynamic) {
            for (std::size_t i = 0; i < driver.size(); i += 4)
                if (std::memcmp(&driver[i], &dynamic[i], 4)) {
                    std::fprintf(stderr, "%u samples: pixel (%zu, %zu) driver %u,%u,%u dynamic %u,%u,%u\n", samples,
                                 (i % 256) / 4, i / 256, driver[i], driver[i + 1], driver[i + 2], dynamic[i],
                                 dynamic[i + 1], dynamic[i + 2]);
                    break;
                }
            return 1;
        }
        ++counts;
    }

    // 2. Custom positions for every pixel.
    std::vector<D3D12_SAMPLE_POSITION> custom = {{-7, -3}, {5, -6}, {-2, 6}, {3, 2}};
    std::vector<std::uint8_t> pixels;
    EXPECT(render(d, shaders, d.sample_locations, 4, [&](ID3D12GraphicsCommandList1* l) {
        l->SetSamplePositions(4, 1, custom.data());
    }, pixels));
    unsigned partial_custom = 0;
    EXPECT(!compare_coverage(pixels, 4, 1, custom, partial_custom));
    EXPECT(partial_custom > 0);

    // 3. A 2x2 pattern: each pixel of the quad has its own four positions.
    std::vector<D3D12_SAMPLE_POSITION> quad = {{-7, -3}, {5, -6}, {-2, 6}, {3, 2},     // top-left
                                               {-6, -6}, {-5, -4}, {-4, -6}, {-6, -5},  // top-right: all low
                                               {6, 5}, {5, 6}, {4, 4}, {6, 4},          // bottom-left: all high
                                               {-8, 7}, {7, -8}, {0, 3}, {-3, -2}};     // bottom-right
    unsigned partial_quad = 0, quads = 0;
    VkMultisamplePropertiesEXT multisample{VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT};
    d.instance_fn<PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT>("vkGetPhysicalDeviceMultisamplePropertiesEXT")(
        d.physical, VK_SAMPLE_COUNT_4_BIT, &multisample);
    if (multisample.maxSampleLocationGridSize.width >= 2 && multisample.maxSampleLocationGridSize.height >= 2) {
        EXPECT(render(d, shaders, d.sample_locations, 4, [&](ID3D12GraphicsCommandList1* l) {
            l->SetSamplePositions(4, 4, quad.data());
        }, pixels));
        EXPECT(!compare_coverage(pixels, 4, 2, quad, partial_quad));
        ++quads;
    }

    // 4. Back to the standard positions.
    std::vector<std::uint8_t> standard, reset;
    EXPECT(render(d, shaders, 0, 4, nullptr, standard));
    EXPECT(render(d, shaders, d.sample_locations, 4, [&](ID3D12GraphicsCommandList1* l) {
        l->SetSamplePositions(4, 1, custom.data());
        l->SetSamplePositions(0, 0, nullptr);
    }, reset));
    EXPECT(standard == reset);
    EXPECT(standard != pixels);  // the custom image really differs

    // 5. Invalid calls and a count that is not the PSO's.
    std::vector<D3D12_SAMPLE_POSITION> out_of_range = {{8, 0}, {0, 0}, {0, 0}, {0, 0}};
    EXPECT(render(d, shaders, d.sample_locations, 4, [&](ID3D12GraphicsCommandList1* l) {
        l->SetSamplePositions(4, 2, custom.data());
    }, pixels, leonida::ReplayResult::success, E_INVALIDARG));
    EXPECT(render(d, shaders, d.sample_locations, 4, [&](ID3D12GraphicsCommandList1* l) {
        l->SetSamplePositions(4, 1, out_of_range.data());
    }, pixels, leonida::ReplayResult::success, E_INVALIDARG));
    EXPECT(render(d, shaders, d.sample_locations, 4, [&](ID3D12GraphicsCommandList1* l) {
        l->SetSamplePositions(2, 1, custom.data());
    }, pixels, leonida::ReplayResult::invalid_subresource));

    // 6. Single-sampled positions (Tier 2; @Vonsek's case on #848): the centre, then (-6, -6) and (+6, +6).
    unsigned single_cases = 0;
    if (d.sample_locations & VK_SAMPLE_COUNT_1_BIT) {
        std::vector<std::uint8_t> driver_1x, dynamic_1x;
        EXPECT(render(d, shaders, 0, 1, nullptr, driver_1x));
        EXPECT(render(d, shaders, d.sample_locations, 1, nullptr, dynamic_1x));
        EXPECT(driver_1x == dynamic_1x);  // the standard 1x position is the centre on both paths
        unsigned partial_1x = 0;
        for (const D3D12_SAMPLE_POSITION shift : {D3D12_SAMPLE_POSITION{-6, -6}, D3D12_SAMPLE_POSITION{6, 6}}) {
            std::vector<D3D12_SAMPLE_POSITION> one = {shift};
            EXPECT(render(d, shaders, d.sample_locations, 1, [&](ID3D12GraphicsCommandList1* l) {
                l->SetSamplePositions(1, 1, one.data());
            }, pixels));
            EXPECT(!compare_coverage(pixels, 1, 1, one, partial_1x));
            EXPECT(pixels != driver_1x);  // the shift moved the edge
            ++single_cases;
        }
    }

    // 7. D3D12_CENTER_MULTISAMPLE_PATTERN at 4x: every sample at the centre, so each pixel is all or nothing.
    const std::vector<D3D12_SAMPLE_POSITION> centre(4, D3D12_SAMPLE_POSITION{0, 0});
    EXPECT(render(d, shaders, d.sample_locations, 4, nullptr, pixels, leonida::ReplayResult::success, S_OK,
                  UINT(D3D12_CENTER_MULTISAMPLE_PATTERN)));
    unsigned partial_centre = 0;
    EXPECT(!compare_coverage(pixels, 4, 1, centre, partial_centre));
    EXPECT(partial_centre == 0);

    std::filesystem::remove_all(shaders.library);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("sample positions: standard tables match the driver at %u counts; custom positions (%u partial "
                "pixels) and %s; %u single-sample positions and the centre pattern match the computed coverage; "
                "reset, invalid calls and a mismatched count behave\n",
                counts, partial_custom, quads ? "a 2x2 pattern match the computed coverage" : "no 2x2 grid here",
                single_cases);
    return 0;
}
