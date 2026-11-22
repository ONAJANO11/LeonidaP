// OMSetDepthBounds on the GPU, through the Xbox-ABI proxy over the Vulkan device. A D32_FLOAT depth target is
// cleared to 0.25 on its left half and 0.75 on its right half; the colour target is cleared blue. Four draws of the
// d3d12_draw_test quad, one per quadrant, with depth and stencil tests off:
//   - top-left, depth-bounds PSO, bounds [0.5, 1]: the stored 0.25 is outside, nothing drawn (blue);
//   - top-right, depth-bounds PSO, bounds [0.5, 1]: 0.75 is inside, drawn red;
//   - bottom-left, a PSO without the test, bounds still [0.5, 1]: the bounds are ignored, drawn green;
//   - bottom-right, depth-bounds PSO, bounds (-1 -> 0, 0.5): 0.75 is outside, nothing drawn (blue).
// The test reads the stored depth, not the fragment's, so it works with DepthEnable FALSE (as in D3D12 and Vulkan).
// The depth-bounds PSO comes from create_graphics_pipeline_state with GraphicsPipelineExtras, what a pipeline state
// stream with D3D12_DEPTH_STENCIL_DESC1::DepthBoundsTestEnable maps to. Without the depthBounds feature that PSO
// must be refused, and the pixel part is skipped. Shaders: test/glsl/vs_draw.vert, ps_draw.frag. No game data.
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

    const auto vs_program = fssm_program({0x56530001, 0xbf810000});
    const auto ps_program = fssm_program({0x50530001, 0xbf810000});
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program);
    const auto library = std::filesystem::temp_directory_path() / "leonida_depth_bounds_test";
    std::filesystem::create_directories(library);
    std::filesystem::copy_file(LEONIDA_TEST_VS_SPV, library / "vs_draw.spv", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_PS_SPV, library / "ps_draw.spv", std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"vs_draw\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": ["
                 "{\"semantic\": \"COLOR\", \"semantic_index\": 0, \"register\": 0, \"mask\": 15, \"rw_mask\": 15, "
                 "\"system_value\": 0, \"component_type\": \"float\"}, "
                 "{\"semantic\": \"POSITION\", \"semantic_index\": 0, \"register\": 1, \"mask\": 3, \"rw_mask\": 3, "
                 "\"system_value\": 0, \"component_type\": \"float\"}]},\n"
                 " \"ps_draw\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }

    constexpr UINT size = 64;
    std::vector<std::uint8_t> pixels(size * 256), second(size * 256);
    bool skipped = false;
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.features.depthBounds = d.enabled.depthBounds;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        // Root signature as in d3d12_draw_test: [0] two constants (offset), [1] a root CBV (tint).
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
        pso_desc.DepthStencilState.DepthEnable = FALSE;  // the depth-bounds test does not need the depth test
        pso_desc.InputLayout = {elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso_desc.SampleDesc.Count = 1;
        rt::d3d12::GraphicsPipelineExtras bounds_on{};
        bounds_on.depth_bounds_test = true;
        Ref<ID3D12PipelineState> plain, bounded;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&plain.p))));
        const HRESULT bounded_hr =
            rt::d3d12::create_graphics_pipeline_state(device.p, pso_desc, bounds_on, IID_PPV_ARGS(&bounded.p));
        if (!d.enabled.depthBounds) {
            // D3D12 refuses such a PSO where OPTIONS2.DepthBoundsTestSupported is FALSE.
            EXPECT(bounded_hr == E_INVALIDARG && !bounded.p);
            skipped = true;
        } else {
            EXPECT(SUCCEEDED(bounded_hr) && bounded.p);
        }
        // Extras left at zero are the plain desc.
        Ref<ID3D12PipelineState> zero_extras;
        EXPECT(SUCCEEDED(rt::d3d12::create_graphics_pipeline_state(device.p, pso_desc, {}, IID_PPV_ARGS(&zero_extras.p))));
        ID3D12PipelineState* no_device = reinterpret_cast<ID3D12PipelineState*>(1);
        EXPECT(rt::d3d12::create_graphics_pipeline_state(nullptr, pso_desc, {}, IID_PPV_ARGS(&no_device)) ==
               E_INVALIDARG && !no_device);

        if (!skipped) {
            // The d3d12_draw_test quad in the top-left quadrant; root constants move it to the others.
            const Vertex vertices[4] = {
                {-1, 1, 255, 0, 0, 255}, {0, 1, 255, 0, 0, 255}, {-1, 0, 255, 0, 0, 255}, {0, 0, 255, 0, 0, 255}};
            const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
            const float tints[2][3] = {{1, 0, 1}, {0, 1, 0}};  // red stays red; then green

            const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
            const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
            const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
            Ref<ID3D12Resource> vertex_buffer, upload, readback, target, depth;
            const auto vb_desc = buffer_desc(sizeof(vertices));
            EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&vertex_buffer.p))));
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
                                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(&target.p))));
            D3D12_RESOURCE_DESC ds_desc = rt_desc;
            ds_desc.Format = DXGI_FORMAT_D32_FLOAT;
            ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &ds_desc,
                                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                                             IID_PPV_ARGS(&depth.p))));

            std::uint8_t* up = nullptr;
            EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
            std::memcpy(up, vertices, sizeof(vertices));
            std::memcpy(up + 256, indices, sizeof(indices));
            std::memcpy(up + 512, tints[0], sizeof(tints[0]));
            std::memcpy(up + 768, tints[1], sizeof(tints[1]));
            const D3D12_GPU_VIRTUAL_ADDRESS upload_va = upload->GetGPUVirtualAddress();

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
            Ref<ID3D12GraphicsCommandList1> list1;
            Ref<ID3D12Fence> fence;
            EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
            EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator.p))));
            EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, bounded.p,
                                                       IID_PPV_ARGS(&list.p))));
            EXPECT(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list1.p))));
            EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

            list->CopyBufferRegion(vertex_buffer.p, 0, upload.p, 0, sizeof(vertices));
            D3D12_RESOURCE_BARRIER barriers[2] = {
                transition(vertex_buffer.p, D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
                transition(target.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
            list->ResourceBarrier(2, barriers);
            const FLOAT blue[4] = {0, 0, 1, 1};
            list->ClearRenderTargetView(rtv, blue, 0, nullptr);
            const D3D12_RECT left{0, 0, LONG(size / 2), LONG(size)}, right{LONG(size / 2), 0, LONG(size), LONG(size)};
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 1, &left);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 1, &right);

            list->SetGraphicsRootSignature(root.p);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D12_VERTEX_BUFFER_VIEW vbv{vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices),
                                               sizeof(Vertex)};
            list->IASetVertexBuffers(0, 1, &vbv);
            const D3D12_INDEX_BUFFER_VIEW ibv{upload_va + 256, sizeof(indices), DXGI_FORMAT_R16_UINT};
            list->IASetIndexBuffer(&ibv);
            const D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

            const float top_left[2] = {0, 0}, top_right[2] = {1, 0}, bottom_left[2] = {0, -1},
                        bottom_right[2] = {1, -1};
            list1->OMSetDepthBounds(0.5f, 1.0f);
            list->SetGraphicsRootConstantBufferView(1, upload_va + 512);  // red
            list->SetGraphicsRoot32BitConstants(0, 2, top_left, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);  // 0.25 outside [0.5, 1]: blue stays
            list->SetGraphicsRoot32BitConstants(0, 2, top_right, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);  // 0.75 inside: red
            list->SetPipelineState(plain.p);
            list->SetGraphicsRootConstantBufferView(1, upload_va + 768);  // green
            list->SetGraphicsRoot32BitConstants(0, 2, bottom_left, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);  // no test in this PSO: green
            list->SetPipelineState(bounded.p);
            list1->OMSetDepthBounds(-1.0f, 0.5f);       // clamped to [0, 0.5]
            list->SetGraphicsRoot32BitConstants(0, 2, bottom_right, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);  // 0.75 outside [0, 0.5]: blue stays

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
            readback->Unmap(0, nullptr);

            // Second list, same depth target (0.25 left, 0.75 right): equal bounds, and an inverted interval, which
            // D3D12 fails for every sample and Vulkan may not be given (VUID-vkCmdSetDepthBounds-minDepthBounds-10912).
            EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, bounded.p)));
            D3D12_RESOURCE_BARRIER to_target = transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &to_target);
            list->ClearRenderTargetView(rtv, blue, 0, nullptr);
            list->SetGraphicsRootSignature(root.p);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &vbv);
            list->IASetIndexBuffer(&ibv);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            list->SetGraphicsRootConstantBufferView(1, upload_va + 512);  // red
            list1->OMSetDepthBounds(0.25f, 0.25f);
            list->SetGraphicsRoot32BitConstants(0, 2, top_left, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);  // 0.25 in [0.25, 0.25]: red
            list1->OMSetDepthBounds(0.75f, 0.25f);
            list->SetGraphicsRoot32BitConstants(0, 2, top_right, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);  // inverted: every sample fails, blue stays
            list1->OMSetDepthBounds(0.75f, 0.75f);
            list->SetGraphicsRoot32BitConstants(0, 2, bottom_left, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);  // 0.25 outside [0.75, 0.75]: blue stays
            list1->OMSetDepthBounds(0.75f, 0.25f);
            list->SetPipelineState(plain.p);
            list->SetGraphicsRootConstantBufferView(1, upload_va + 768);  // green
            list->SetGraphicsRoot32BitConstants(0, 2, bottom_right, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);  // no test in this PSO: inverted bounds ignored, green
            list->ResourceBarrier(1, &to_copy);
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            EXPECT(list->Close() == S_OK);
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
            EXPECT(SUCCEEDED(queue->Signal(fence.p, 2)));
            EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, nullptr)));
            EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
            std::memcpy(second.data(), mapped, second.size());

            for (auto* r : {&vertex_buffer, &upload, &readback, &target, &depth}) r->reset();
            rtv_heap.reset();
            dsv_heap.reset();
            list1.reset();
            list.reset();
            allocator.reset();
            queue.reset();
            fence.reset();
        }
        plain.reset();
        bounded.reset();
        zero_extras.reset();
        root.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }
    std::filesystem::remove_all(library);

    if (skipped) {
        std::printf("SKIP: no depthBounds feature on this GPU; the depth-bounds PSO was refused with E_INVALIDARG\n");
        return d.validation_errors() ? 1 : 77;  // ctest's SKIP_RETURN_CODE
    }
    unsigned mismatches = 0;
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const bool top = row < size / 2, left = col < size / 2;
            const std::array<std::uint8_t, 4> want = top && !left   ? std::array<std::uint8_t, 4>{255, 0, 0, 255}
                                                     : !top && left ? std::array<std::uint8_t, 4>{0, 255, 0, 255}
                                                                    : std::array<std::uint8_t, 4>{0, 0, 255, 255};
            const std::uint8_t* got = pixels.data() + row * 256 + col * 4;
            if (std::equal(want.begin(), want.end(), got)) continue;
            if (mismatches++ < 6)
                std::fprintf(stderr, "pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row, got[0], got[1],
                             got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    }
    EXPECT(!mismatches);
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const bool top = row < size / 2, left = col < size / 2;
            const std::array<std::uint8_t, 4> want = top && left     ? std::array<std::uint8_t, 4>{255, 0, 0, 255}
                                                     : !top && !left ? std::array<std::uint8_t, 4>{0, 255, 0, 255}
                                                                     : std::array<std::uint8_t, 4>{0, 0, 255, 255};
            const std::uint8_t* got = second.data() + row * 256 + col * 4;
            if (std::equal(want.begin(), want.end(), got)) continue;
            if (mismatches++ < 6)
                std::fprintf(stderr, "second list, pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row,
                             got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    }
    EXPECT(!mismatches);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("depth bounds: eight draws matched all %u pixels twice: bounds [0.5, 1] reject 0.25 and pass 0.75, a "
                "PSO without the test ignores them, clamped [0, 0.5] rejects 0.75, equal bounds pass only their value, "
                "an inverted interval fails every sample\n",
                size * size);
    return 0;
}
