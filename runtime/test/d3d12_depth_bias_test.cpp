// RSSetDepthBias (ID3D12GraphicsCommandList9, the console RSSetDepthBiasX's public form) as dynamic depth-bias
// state on the Vulkan layer, stacked on the draw path. A full-screen triangle whose z rises linearly along x is
// drawn against a depth buffer cleared to 0.5 with a GREATER depth test, and where the pass line lands tells the
// effective bias exactly:
//   - the PSO's static bias (RasterizerState) when the list set nothing;
//   - RSSetDepthBias replacing it (a huge constant factor, so the clamp alone lands: applied bias == clamp);
//   - the value persisting across SetPipelineState, and ClearState / Reset restoring the PSO's;
//   - a PSO without D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS ignoring the call;
//   - an explicit zero override (no bias) and a slope-scaled bias (bounds only: the depth slope the factor
//     multiplies is implementation-approximate, so exact columns are not portable there).
// The shaders are GLSL written to the translator's contract (test/glsl/vs_slope.vert, ps_solid.frag). No game data.
// Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#if !defined(LEONIDA_TEST_VS_SLOPE_SPV) || !defined(LEONIDA_TEST_PS_SOLID_SPV)
#error "LEONIDA_TEST_VS_SLOPE_SPV and LEONIDA_TEST_PS_SOLID_SPV must name the compiled test shaders"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

namespace {

constexpr UINT target_size = 64;

// The triangle's z at texel column i (centres) is 0.25 + (2i + 1) / 256, so a bias of k/256 moves the GREATER-0.5
// pass line by k/2 columns. k even keeps every comparison strictly off the tie.
constexpr float texel_k(int k) { return k / 256.0f; }

// Columns that pass with bias k/256 added: 2i + 1 > 64 - k, so i > (63 - k) / 2.
constexpr UINT passing_columns(int k) { return target_size - UINT((63 - k) / 2) - 1; }

struct SlopeVertex {
    float x, y, z;
};

}  // namespace

int main() {
    vktest::Device d;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;
    if (!d.enabled.depthBiasClamp) {
        std::printf("depthBiasClamp is not available on this device: SKIP\n");
        return 77;
    }

    const auto vs_program = fssm_program({0x56530003, 0xbf810000});
    const auto ps_program = fssm_program({0x50530002, 0xbf810000});
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program);
    const auto library = std::filesystem::temp_directory_path() / "leonida_depth_bias_test";
    std::filesystem::create_directories(library);
    std::filesystem::copy_file(LEONIDA_TEST_VS_SLOPE_SPV, library / "vs_slope.spv",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_PS_SOLID_SPV, library / "ps_solid.spv",
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"vs_slope\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": ["
                 "{\"semantic\": \"POSITION\", \"semantic_index\": 0, \"register\": 0, \"mask\": 7, \"rw_mask\": 7, "
                 "\"system_value\": 0, \"component_type\": \"float\"}]},\n"
                 " \"ps_solid\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }

    unsigned scenarios = 0, failures = 0;
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.shader_dir = library.string();
        info.features.depthBiasClamp = d.enabled.depthBiasClamp;
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        // An empty root signature: the shaders read no root arguments.
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

        // Three pipelines over one recipe. The PSO's static bias is a huge constant factor with a small clamp, so
        // the applied bias is exactly the clamp. dyn_a and dyn_b take RSSetDepthBias; plain does not.
        const D3D12_INPUT_ELEMENT_DESC elements[1] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC base{};
        base.pRootSignature = root.p;
        base.VS = {vs_container.data(), vs_container.size()};
        base.PS = {ps_container.data(), ps_container.size()};
        base.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        base.SampleMask = 0xffffffff;
        base.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        base.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        base.RasterizerState.DepthClipEnable = TRUE;
        base.RasterizerState.DepthBias = 2000000000;  // the clamp alone lands: applied bias == DepthBiasClamp
        base.DepthStencilState.DepthEnable = TRUE;
        base.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        base.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
        base.InputLayout = {elements, 1};
        base.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        base.NumRenderTargets = 1;
        base.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        base.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        base.SampleDesc.Count = 1;
        Ref<ID3D12PipelineState> dyn_a, dyn_b, plain;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = base;
        desc.Flags = D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS;
        desc.RasterizerState.DepthBiasClamp = texel_k(10);
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&dyn_a.p))));
        desc.RasterizerState.DepthBiasClamp = texel_k(30);
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&dyn_b.p))));
        desc = base;
        desc.RasterizerState.DepthBiasClamp = texel_k(30);  // no flag: RSSetDepthBias must not touch it
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&plain.p))));

        // The triangle: z = 0.25 + (x + 1) / 4, clockwise in D3D screen space, covering the screen.
        const SlopeVertex vertices[3] = {{-1, -1, 0.25f}, {-1, 3, 0.25f}, {3, -1, 1.25f}};

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> upload, readback, target, depth;
        const D3D12_RESOURCE_DESC upload_desc = buffer_desc(sizeof(vertices) + 256);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload.p))));
        const D3D12_RESOURCE_DESC readback_desc = buffer_desc(target_size * 256);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = target_size;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&target.p))));
        D3D12_RESOURCE_DESC depth_desc = rt_desc;
        depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cleared_to{};
        cleared_to.Format = DXGI_FORMAT_D32_FLOAT;
        cleared_to.DepthStencil.Depth = 0.5f;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &cleared_to,
                                                         IID_PPV_ARGS(&depth.p))));
        std::uint8_t* up = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
        std::memcpy(up + 128, vertices, sizeof(vertices));

        const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1,
                                                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p))};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(dsv_heap.p, 9))(dsv_heap.p))};
        device->CreateRenderTargetView(target.p, nullptr, rtv);
        device->CreateDepthStencilView(depth.p, nullptr, dsv);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, dyn_a.p,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        const D3D12_VERTEX_BUFFER_VIEW vbv{upload->GetGPUVirtualAddress() + 128, sizeof(vertices),
                                           sizeof(SlopeVertex)};
        const D3D12_VIEWPORT viewport{0, 0, float(target_size), float(target_size), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(target_size), LONG(target_size)};
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, target_size, target_size, 1, 256}};
        src.pResource = target.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const FLOAT blue[4] = {0, 0, 1, 1};
        // The colour target is created COMMON and then lives COPY_SOURCE <-> RENDER_TARGET between submissions.
        // The depth buffer is created DEPTH_WRITE and stays there: only clears and depth tests touch it.
        const D3D12_RESOURCE_BARRIER to_render_first[] = {
            transition(target.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        const D3D12_RESOURCE_BARRIER to_render[] = {
            transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        const D3D12_RESOURCE_BARRIER to_copy[] = {
            transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        ID3D12CommandList* lists[] = {list.p};
        std::vector<std::uint8_t> pixels(target_size * 256);
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        UINT signal = 0;

        // The list state every scenario shares; the per-scenario part only varies what it tests.
        const auto shared_state = [&] {
            list->SetGraphicsRootSignature(root.p);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &vbv);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        };
        // The helpers return 0 on success and 1 from a failed EXPECT; every call site checks it.
        const auto begin_pass = [&](ID3D12PipelineState* initial, bool first) -> int {
            if (!first) {  // a fresh list from CreateCommandList is already recording, with `initial` bound
                EXPECT(SUCCEEDED(allocator->Reset()));
                EXPECT(SUCCEEDED(list->Reset(allocator.p, initial)));
            }
            list->ResourceBarrier(1, first ? to_render_first : to_render);
            list->ClearRenderTargetView(rtv, blue, 0, nullptr);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
            shared_state();
            return 0;
        };
        const auto end_pass = [&]() -> int {
            list->DrawInstanced(3, 1, 0, 0);
            list->ResourceBarrier(1, to_copy);
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            EXPECT(list->Close() == S_OK);
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
            EXPECT(SUCCEEDED(queue->Signal(fence.p, ++signal)));
            EXPECT(SUCCEEDED(fence->SetEventOnCompletion(signal, nullptr)));
            std::memcpy(pixels.data(), mapped, pixels.size());
            return 0;
        };
        const auto set_bias = [&](float constant, float clamp, float slope) -> int {
            ID3D12GraphicsCommandList9* gcl9 = nullptr;
            EXPECT(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&gcl9))) && gcl9);
            gcl9->RSSetDepthBias(constant, clamp, slope);
            gcl9->Release();
            return 0;
        };
        // Every pixel must be red or blue, and red exactly in the last `want` columns.
        const auto check = [&](const char* what, UINT want) {
            unsigned bad = 0;
            for (UINT row = 0; row < target_size && bad < 6; ++row) {
                for (UINT col = 0; col < target_size && bad < 6; ++col) {
                    const std::uint8_t* got = pixels.data() + row * 256 + col * 4;
                    const bool red = got[0] == 255 && got[1] == 0 && got[2] == 0 && got[3] == 255;
                    const bool blue_pixel = got[0] == 0 && got[1] == 0 && got[2] == 255 && got[3] == 255;
                    const bool wanted = col >= target_size - want;
                    if ((red != wanted) || (!red && !blue_pixel)) {
                        ++bad;
                        std::fprintf(stderr, "%s: pixel (%u, %u) is %u,%u,%u,%u, want %s\n", what, col, row, got[0],
                                     got[1], got[2], got[3], wanted ? "red" : "blue");
                    }
                }
            }
            if (bad) {
                ++failures;
                return;
            }
            std::printf("  %-42s %u of %u columns pass\n", what, want, target_size);
            ++scenarios;
        };

        std::printf("RSSetDepthBias on ID3D12GraphicsCommandList9: the depth-test pass line\n");
        // The PSO's own RasterizerState applies when the list set nothing: dyn_a clamps to 10 texel columns.
        EXPECT(begin_pass(dyn_a.p, true) == 0);
        EXPECT(end_pass() == 0);
        check("the PSO's static bias", passing_columns(10));
        // RSSetDepthBias replaces it: 20 columns.
        EXPECT(begin_pass(dyn_a.p, false) == 0);
        EXPECT(set_bias(1e30f, texel_k(20), 0.0f) == 0);
        EXPECT(end_pass() == 0);
        check("RSSetDepthBias overrides the PSO", passing_columns(20));
        // The value persists across SetPipelineState: dyn_b's own 30 must not show.
        EXPECT(begin_pass(dyn_a.p, false) == 0);
        EXPECT(set_bias(1e30f, texel_k(20), 0.0f) == 0);
        list->SetPipelineState(dyn_b.p);
        EXPECT(end_pass() == 0);
        check("value persists across SetPipelineState", passing_columns(20));
        // Reset returns the list to the PSO it starts with: dyn_b's static 30.
        EXPECT(begin_pass(dyn_b.p, false) == 0);
        EXPECT(end_pass() == 0);
        check("Reset restores the new PSO's bias", passing_columns(30));
        // ClearState after an override restores the pipeline's own values too.
        EXPECT(begin_pass(dyn_b.p, false) == 0);
        EXPECT(set_bias(1e30f, texel_k(20), 0.0f) == 0);
        list->ClearState(dyn_b.p);
        shared_state();
        EXPECT(end_pass() == 0);
        check("ClearState restores the PSO's bias", passing_columns(30));
        // A pipeline without the flag is untouched by the call: plain's static 30 stands over a 10-column call.
        EXPECT(begin_pass(dyn_a.p, false) == 0);
        EXPECT(set_bias(1e30f, texel_k(10), 0.0f) == 0);
        list->SetPipelineState(plain.p);
        EXPECT(end_pass() == 0);
        check("non-dynamic pipeline ignores the call", passing_columns(30));
        // An explicit zero override draws unbiased.
        EXPECT(begin_pass(dyn_a.p, false) == 0);
        EXPECT(set_bias(0.0f, 0.0f, 0.0f) == 0);
        EXPECT(end_pass() == 0);
        check("explicit zero override", passing_columns(0));
        // The slope-scaled factor: the depth slope it multiplies is implementation-approximate, so only bounds. A
        // factor of 1 over this triangle's slope (1/128 per pixel) is 2 columns of bias, if computed exactly.
        EXPECT(begin_pass(dyn_a.p, false) == 0);
        EXPECT(set_bias(0.0f, 0.0f, 1.0f) == 0);
        EXPECT(end_pass() == 0);
        {
            UINT slope_columns = 0;
            for (UINT col = 0; col < target_size; ++col) {
                const std::uint8_t* got = pixels.data() + (target_size / 2) * 256 + col * 4;
                if (got[0] == 255 && got[1] == 0 && got[2] == 0) ++slope_columns;
            }
            // The shift must be positive (more than the unbiased 32 columns) yet nowhere near saturating.
            EXPECT(slope_columns > passing_columns(0));
            EXPECT(slope_columns <= passing_columns(0) + 6);
            std::printf("  %-42s %u columns pass (unbiased %u, exact %u)\n", "slope-scaled bias", slope_columns,
                        passing_columns(0), passing_columns(2));
            ++scenarios;
        }

        for (auto* r : {&upload, &readback, &target, &depth}) r->reset();
        for (auto* p : {&dyn_a, &dyn_b, &plain}) p->reset();
        root.reset();
        rtv_heap.reset();
        dsv_heap.reset();
        list.reset();
        allocator.reset();
        queue.reset();
        fence.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }
    std::filesystem::remove_all(library);
    EXPECT(!failures);
    EXPECT(scenarios == 8);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("RSSetDepthBias: %u scenarios matched (static, override, persistence, Reset, ClearState, ignored "
                "without the flag, zero, slope bounds) on a %ux%u target\n",
                scenarios, target_size, target_size);
    return 0;
}
