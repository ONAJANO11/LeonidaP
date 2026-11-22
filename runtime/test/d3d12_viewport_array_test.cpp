// Viewport and scissor arrays through the Xbox-ABI proxy over the Vulkan device. RSSetViewports sets four
// viewports, one per quadrant of a 64x64 target cleared green, and RSSetScissorRects three rectangles: the first
// two cover their quadrants, the third only the left half of the bottom-left one. One instanced draw of a
// full-screen quad sends instance i to viewport i (test/glsl/vs_viewports.vert writes gl_ViewportIndex, as a
// translated VS does from its pos1 misc export):
//   - top-left red and top-right blue (instances 0 and 1, whole quadrants);
//   - bottom-left magenta on its left half only (scissor 2), green on the right half;
//   - bottom-right green: viewport 3 has no scissor rectangle, and D3D12's default rectangle is empty.
// A second list takes the one-viewport case of the with-count path, the standalone device's usual one, on the same
// pipeline: one instance into the top-half viewport under a full-screen scissor (the top half red), then
// RSSetScissorRects(0, ...) and one instance into the bottom-half viewport, which draws nothing: a count of 0
// leaves no rectangle set, as on WARP (keeping the old full-screen one would paint the bottom red). With the scissor
// back, RSSetViewports(0, ...) then draws nothing either, as natively on AMD and WARP. A third list sets 17
// viewports, which D3D12 refuses: Close returns E_INVALIDARG. Skipped where the GPU lacks
// multiViewport or shaderOutputViewportIndex. No game data.
// Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"

#include <array>
#include <cstring>

#if !defined(LEONIDA_TEST_VS_SPV) || !defined(LEONIDA_TEST_PS_SPV)
#error "LEONIDA_TEST_VS_SPV and LEONIDA_TEST_PS_SPV must name the compiled test shaders"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

namespace {

struct Vertex {
    float x, y;
    std::uint8_t r, g, b, a;
};

}  // namespace

int main() {
    vktest::Device d;
    EXPECT(d.create());
    if (!d.multi_viewport) {
        std::printf("SKIP: no multiViewport / shaderOutputViewportIndex on this GPU\n");
        return 77;  // ctest's SKIP_RETURN_CODE
    }

    const auto vs_program = fssm_program({0x56530002, 0xbf810000});
    const auto ps_program = fssm_program({0x50530001, 0xbf810000});
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program);
    const auto library = std::filesystem::temp_directory_path() / "leonida_viewport_array_test";
    std::filesystem::create_directories(library);
    std::filesystem::copy_file(LEONIDA_TEST_VS_SPV, library / "vs_viewports.spv",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_PS_SPV, library / "ps_draw.spv", std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"vs_viewports\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": ["
                 "{\"semantic\": \"COLOR\", \"semantic_index\": 0, \"register\": 0, \"mask\": 15, \"rw_mask\": 15, "
                 "\"system_value\": 0, \"component_type\": \"float\"}, "
                 "{\"semantic\": \"POSITION\", \"semantic_index\": 0, \"register\": 1, \"mask\": 3, \"rw_mask\": 3, "
                 "\"system_value\": 0, \"component_type\": \"float\"}]},\n"
                 " \"ps_draw\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }

    constexpr UINT size = 64, half = size / 2;
    std::vector<std::uint8_t> pixels(size * 256), single(size * 256);
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.multi_viewport = true;
        info.pipeline_statistics_query = d.pipeline_statistics_query;
        info.occlusion_query_precise = d.occlusion_query_precise;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor = {1, 0};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

        const D3D12_INPUT_ELEMENT_DESC elements[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.VS = {vs_container.data(), vs_container.size()};
        pso_desc.PS = {ps_container.data(), ps_container.size()};
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = 0xffffffff;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.InputLayout = {elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        Ref<ID3D12PipelineState> pso;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

        const Vertex vertices[4] = {
            {-1, 1, 0, 0, 0, 255}, {1, 1, 0, 0, 0, 255}, {-1, -1, 0, 0, 0, 255}, {1, -1, 0, 0, 0, 255}};
        const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
        const float tint[3] = {1, 0, 1};

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> upload, readback, target;
        const auto upload_desc = buffer_desc(4096);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload.p))));
        const auto readback_desc = buffer_desc(size * 256);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = size;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&target.p))));
        std::uint8_t* up = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
        std::memcpy(up, vertices, sizeof(vertices));
        std::memcpy(up + 256, indices, sizeof(indices));
        std::memcpy(up + 512, tint, sizeof(tint));
        const D3D12_GPU_VIRTUAL_ADDRESS upload_va = upload->GetGPUVirtualAddress();

        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> rtv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p))};
        device->CreateRenderTargetView(target.p, nullptr, rtv);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso.p,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        const FLOAT green[4] = {0, 1, 0, 1};
        list->ClearRenderTargetView(rtv, green, 0, nullptr);
        list->SetGraphicsRootSignature(root.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{upload_va, sizeof(vertices), sizeof(Vertex)};
        list->IASetVertexBuffers(0, 1, &vbv);
        const D3D12_INDEX_BUFFER_VIEW ibv{upload_va + 256, sizeof(indices), DXGI_FORMAT_R16_UINT};
        list->IASetIndexBuffer(&ibv);
        const float h = float(half);
        const D3D12_VIEWPORT viewports[4] = {{0, 0, h, h, 0, 1}, {h, 0, h, h, 0, 1}, {0, h, h, h, 0, 1},
                                             {h, h, h, h, 0, 1}};
        const LONG hl = LONG(half), sl = LONG(size);
        const D3D12_RECT scissors[3] = {{0, 0, hl, hl}, {hl, 0, sl, hl}, {0, hl, hl / 2, sl}};
        list->RSSetViewports(4, viewports);
        list->RSSetScissorRects(3, scissors);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float origin[2] = {0, 0};
        list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
        list->DrawIndexedInstanced(6, 4, 0, 0, 0);

        D3D12_RESOURCE_BARRIER to_copy = transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, 256}};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        EXPECT(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        std::memcpy(pixels.data(), mapped, pixels.size());

        // One viewport at a time: the top half under a full-screen scissor, then the bottom half with no scissor.
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso.p)));
        D3D12_RESOURCE_BARRIER back = transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &back);
        list->ClearRenderTargetView(rtv, green, 0, nullptr);
        list->SetGraphicsRootSignature(root.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->IASetIndexBuffer(&ibv);
        const D3D12_VIEWPORT top_half{0, 0, float(size), h, 0, 1}, bottom_half{0, h, float(size), h, 0, 1};
        const D3D12_RECT full_rect{0, 0, sl, sl};
        list->RSSetViewports(1, &top_half);
        list->RSSetScissorRects(1, &full_rect);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
        list->DrawIndexedInstanced(6, 1, 0, 0, 0);
        list->RSSetScissorRects(0, &full_rect);  // a valid pointer, as @Vonsek's case on #873
        list->RSSetViewports(1, &bottom_half);
        list->DrawIndexedInstanced(6, 1, 0, 0, 0);
        list->RSSetScissorRects(1, &full_rect);  // the scissor back, then no viewport: still nothing
        list->RSSetViewports(0, &bottom_half);
        list->DrawIndexedInstanced(6, 1, 0, 0, 0);
        list->ResourceBarrier(1, &to_copy);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        EXPECT(list->Close() == S_OK);
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 2)));
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, nullptr)));
        std::memcpy(single.data(), mapped, single.size());

        // Queries (@Vonsek's cases on #873). A draw with no viewport bound rasterizes nothing but still runs its
        // vertex work, so its pipeline statistics match the same draw with a valid viewport, apart from the pixel
        // shader. And with no attachment bound, the render area covers every viewport: one instance into a 32x32
        // viewport 1 behind an empty viewport 0 counts 1024 occlusion samples.
        unsigned query_checks = 0;
        {
            Ref<ID3D12QueryHeap> statistics, occlusion;
            const D3D12_QUERY_HEAP_DESC stats_desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 2, 0};
            const D3D12_QUERY_HEAP_DESC occl_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1, 0};
            if (d.pipeline_statistics_query)
                EXPECT(SUCCEEDED(device->CreateQueryHeap(&stats_desc, IID_PPV_ARGS(&statistics.p))));
            EXPECT(SUCCEEDED(device->CreateQueryHeap(&occl_desc, IID_PPV_ARGS(&occlusion.p))));
            D3D12_GRAPHICS_PIPELINE_STATE_DESC no_target = pso_desc;  // no render target: occlusion only
            no_target.NumRenderTargets = 0;
            no_target.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
            Ref<ID3D12PipelineState> pso_none;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&no_target, IID_PPV_ARGS(&pso_none.p))));
            Ref<ID3D12Resource> query_rb;
            const auto query_rb_desc = buffer_desc(256);
            EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &query_rb_desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&query_rb.p))));
            EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso.p)));
            D3D12_RESOURCE_BARRIER to_rt = transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &to_rt);
            list->SetGraphicsRootSignature(root.p);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &vbv);
            list->IASetIndexBuffer(&ibv);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
            list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
            const D3D12_VIEWPORT full_vp{0, 0, float(size), float(size), 0, 1};
            const D3D12_RECT all{0, 0, sl, sl};
            list->RSSetScissorRects(1, &all);
            if (statistics.p) {
                list->RSSetViewports(1, &full_vp);
                list->BeginQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
                list->DrawIndexedInstanced(6, 1, 0, 0, 0);
                list->EndQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
                list->RSSetViewports(0, &full_vp);
                list->BeginQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
                list->DrawIndexedInstanced(6, 1, 0, 0, 0);
                list->EndQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
                list->ResolveQueryData(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 2, query_rb.p, 0);
            }
            list->SetPipelineState(pso_none.p);
            list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
            const D3D12_VIEWPORT empty_then_second[2] = {{0, 0, 0, 0, 0, 1}, {h, h, h, h, 0, 1}};
            const D3D12_RECT both[2] = {all, all};
            list->RSSetViewports(2, empty_then_second);
            list->RSSetScissorRects(2, both);
            list->BeginQuery(occlusion.p, D3D12_QUERY_TYPE_OCCLUSION, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 1);  // instance 1: viewport 1
            list->EndQuery(occlusion.p, D3D12_QUERY_TYPE_OCCLUSION, 0);
            list->ResolveQueryData(occlusion.p, D3D12_QUERY_TYPE_OCCLUSION, 0, 1, query_rb.p, 192);
            EXPECT(list->Close() == S_OK);
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
            EXPECT(SUCCEEDED(queue->Signal(fence.p, 3)));
            EXPECT(SUCCEEDED(fence->SetEventOnCompletion(3, nullptr)));
            std::uint8_t* q = nullptr;
            EXPECT(SUCCEEDED(query_rb->Map(0, nullptr, reinterpret_cast<void**>(&q))));
            if (statistics.p) {
                D3D12_QUERY_DATA_PIPELINE_STATISTICS s[2];
                std::memcpy(s, q, sizeof(s));
                std::printf("statistics: valid viewport IA %llu/%llu VS %llu PS %llu; no viewport IA %llu/%llu VS %llu "
                            "PS %llu\n", s[0].IAVertices, s[0].IAPrimitives, s[0].VSInvocations, s[0].PSInvocations,
                            s[1].IAVertices, s[1].IAPrimitives, s[1].VSInvocations, s[1].PSInvocations);
                EXPECT(s[0].IAVertices == 6 && s[0].IAPrimitives == 2 && s[0].VSInvocations > 0 && s[0].PSInvocations);
                EXPECT(s[1].IAVertices == s[0].IAVertices && s[1].IAPrimitives == s[0].IAPrimitives &&
                       s[1].VSInvocations == s[0].VSInvocations && s[1].PSInvocations == 0);
                ++query_checks;
            }
            std::uint64_t samples_passed = 0;
            std::memcpy(&samples_passed, q + 192, sizeof(samples_passed));
            std::printf("occlusion: %llu samples through viewport 1 with no attachment\n",
                        static_cast<unsigned long long>(samples_passed));
            if (d.occlusion_query_precise) EXPECT(samples_passed == 1024);
            else EXPECT(samples_passed != 0);
            ++query_checks;
            query_rb->Unmap(0, nullptr);
        }

        // 17 viewports: more than D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE.
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso.p)));
        D3D12_VIEWPORT many[17] = {};
        for (auto& v : many) v = viewports[0];
        list->RSSetViewports(17, many);
        EXPECT(list->Close() == E_INVALIDARG);

        for (auto* r : {&upload, &readback, &target}) r->reset();
        pso.reset();
        root.reset();
        rtv_heap.reset();
        list.reset();
        allocator.reset();
        queue.reset();
        fence.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }
    std::filesystem::remove_all(library);

    unsigned mismatches = 0;
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const bool top = row < half, left = col < half;
            std::array<std::uint8_t, 4> want{0, 255, 0, 255};                      // green: not drawn
            if (top && left) want = {255, 0, 0, 255};                              // viewport 0: red
            else if (top) want = {0, 0, 255, 255};                                 // viewport 1: blue
            else if (left && col < half / 2) want = {255, 0, 255, 255};            // viewport 2, scissor 2: magenta
            const std::uint8_t* got = pixels.data() + row * 256 + col * 4;
            if (std::equal(want.begin(), want.end(), got)) continue;
            if (mismatches++ < 6)
                std::fprintf(stderr, "pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row, got[0], got[1],
                             got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    }
    EXPECT(!mismatches);
    for (UINT row = 0; row < size; ++row)
        for (UINT col = 0; col < size; ++col) {
            const std::uint8_t* got = single.data() + row * 256 + col * 4;
            const std::array<std::uint8_t, 4> want = row < half ? std::array<std::uint8_t, 4>{255, 0, 0, 255}
                                                                : std::array<std::uint8_t, 4>{0, 255, 0, 255};
            if (!std::equal(want.begin(), want.end(), got) && mismatches++ < 6)
                std::fprintf(stderr, "single viewport, pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row,
                             got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    EXPECT(!mismatches);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("viewport arrays: 4 viewports and 3 scissors matched all %u pixels; instance i drew into viewport i, "
                "scissor 2 trimmed its quadrant, viewport 3 without a scissor drew nothing; one viewport drew the top half and a "
                "scissor count of 0 left the bottom empty; "
                "17 viewports refused\n",
                size * size);
    return 0;
}
