// Milestone M1 (docs/d3d12_vulkan.md section 6) through the Xbox-ABI proxy over the Vulkan device:
//   - a graphics PSO whose VS and PS come from containers shaped like the game's, found in a translator-style
//     index by the SHA-256 of their FSSM programs; the input layout (POSITION, COLOR) is matched to the VS input
//     signature by semantic, and the VS declares them in the opposite register order;
//   - a vertex buffer copied into the GPU address arena, an index buffer read straight from an upload heap, root
//     constants (a clip-space offset) and a root CBV (a tint) through the 32 user-data dwords;
//   - a render target cleared blue, two indexed draws with different root arguments (top-left red, bottom-right
//     green), and one back-facing triangle (bottom-left) that culling must remove;
//   - the image read back through CopyTextureRegion and compared pixel by pixel. Where the quadrants land also
//     checks the viewport flip (D3D clip space is y-up) and the front-face winding.
// The shaders are GLSL written to the translator's contract (test/glsl/vs_draw.vert, ps_draw.frag). No game data.
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
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(device_status == vktest::DeviceStatus::ready);

    // Shader programs and the index that maps their hashes to the compiled test modules.
    const auto vs_program = fssm_program({0x56530001, 0xbf810000});
    const auto ps_program = fssm_program({0x50530001, 0xbf810000});
    const auto fullscreen_program = fssm_program({0x56530002, 0xbf810000});
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program);
    const auto fullscreen_container = shader_container(fullscreen_program);
    const auto library = std::filesystem::temp_directory_path() / "leonida_draw_test";
    std::filesystem::create_directories(library);
    std::filesystem::copy_file(LEONIDA_TEST_VS_SPV, library / "vs_draw.spv", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_PS_SPV, library / "ps_draw.spv", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_VS_FULLSCREEN_SPV, library / "vs_fullscreen.spv",
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"vs_draw\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": ["
                 "{\"semantic\": \"COLOR\", \"semantic_index\": 0, \"register\": 0, \"mask\": 15, \"rw_mask\": 15, "
                 "\"system_value\": 0, \"component_type\": \"float\"}, "
                 "{\"semantic\": \"POSITION\", \"semantic_index\": 0, \"register\": 1, \"mask\": 3, \"rw_mask\": 3, "
                 "\"system_value\": 0, \"component_type\": \"float\"}]},\n"
                 " \"vs_fullscreen\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(fullscreen_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": []},\n"
                 " \"ps_draw\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }

    constexpr UINT size = 64;
    std::vector<std::uint8_t> pixels(size * 256), second_pixels(size * 256);
    std::vector<std::uint8_t> strip16_pixels(size * 256), strip32_pixels(size * 256), strip_nocut_pixels(size * 256);
    std::vector<std::uint8_t> ro_depth_less(size * 256), ro_depth_le(size * 256);
    std::vector<std::uint8_t> ro_stencil_eq1(size * 256), ro_stencil_eq2(size * 256);
    const bool logic_op = d.enabled.logicOp == VK_TRUE;
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
        // Of the PSO features only logicOp is passed on, so the other feature refusals below still apply.
        info.features.logicOp = d.enabled.logicOp;
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        // Root signature: [0] two constants (s0, s1: offset), [1] a root CBV (s2:s3: tint).
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

        // The PSO. Input layout order (POSITION, COLOR) differs from the VS's registers (COLOR 0, POSITION 1).
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
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.InputLayout = {elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        Ref<ID3D12PipelineState> pso, unfed, mrt, instanced, refused, xor_pso, fullscreen;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));
        pso_desc.InputLayout = {elements, 1};  // COLOR missing: D3D12 rejects a layout that leaves an input unfed
        EXPECT(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&unfed.p)) == E_INVALIDARG && !unfed.p);
        pso_desc.InputLayout = {elements, 2};

        // Two render-target formats and a depth format, of which draws bind only RTV 0: a variant with the other
        // formats undefined is used at draw time.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC mrt_desc = pso_desc;
        mrt_desc.NumRenderTargets = 2;
        mrt_desc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
        mrt_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&mrt_desc, IID_PPV_ARGS(&mrt.p))));

        // COLOR per instance from slot 1 with step rate 0: every instance reads element 0.
        const D3D12_INPUT_ELEMENT_DESC instance_elements[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC instance_desc = pso_desc;
        instance_desc.InputLayout = {instance_elements, 2};
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&instance_desc, IID_PPV_ARGS(&instanced.p))));

        // State that needs a Vulkan feature this test device does not enable is refused, not passed to the driver.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC needs_feature = pso_desc;
        needs_feature.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;  // fillModeNonSolid
        EXPECT(device->CreateGraphicsPipelineState(&needs_feature, IID_PPV_ARGS(&refused.p)) == E_NOTIMPL && !refused.p);
        needs_feature = mrt_desc;
        needs_feature.DSVFormat = DXGI_FORMAT_UNKNOWN;
        needs_feature.BlendState.IndependentBlendEnable = TRUE;  // independentBlend: the two targets differ
        needs_feature.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
        EXPECT(device->CreateGraphicsPipelineState(&needs_feature, IID_PPV_ARGS(&refused.p)) == E_NOTIMPL && !refused.p);
        needs_feature = pso_desc;
        needs_feature.RasterizerState.DepthBias = 4;
        needs_feature.RasterizerState.DepthBiasClamp = 0.5f;  // depthBiasClamp
        EXPECT(device->CreateGraphicsPipelineState(&needs_feature, IID_PPV_ARGS(&refused.p)) == E_NOTIMPL && !refused.p);
        needs_feature = pso_desc;
        needs_feature.NumRenderTargets = 9;
        EXPECT(device->CreateGraphicsPipelineState(&needs_feature, IID_PPV_ARGS(&refused.p)) == E_INVALIDARG && !refused.p);
        needs_feature = pso_desc;
        needs_feature.RasterizerState.DepthClipEnable = FALSE;  // depthClamp
        EXPECT(device->CreateGraphicsPipelineState(&needs_feature, IID_PPV_ARGS(&refused.p)) == E_NOTIMPL && !refused.p);

        // XOR on render target 0: applied where the GPU has logicOp (the device passes it on), refused elsewhere.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC xor_desc = pso_desc;
        xor_desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
        xor_desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
        EXPECT(device->CreateGraphicsPipelineState(&xor_desc, IID_PPV_ARGS(&xor_pso.p)) ==
               (logic_op ? S_OK : E_NOTIMPL));
        // A full-screen pass: a VS with no inputs and an empty input layout, so the pipeline has no vertex bindings.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC fullscreen_desc = pso_desc;
        fullscreen_desc.VS = {fullscreen_container.data(), fullscreen_container.size()};
        fullscreen_desc.InputLayout = {nullptr, 0};
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&fullscreen_desc, IID_PPV_ARGS(&fullscreen.p))));

        // Geometry: a quad in the top-left quadrant (front-facing, clockwise in D3D screen space) and a triangle in
        // the bottom-left one wound the other way (back-facing).
        const Vertex vertices[7] = {
            {-1, 1, 255, 0, 0, 255}, {0, 1, 255, 0, 0, 255}, {-1, 0, 255, 0, 0, 255}, {0, 0, 255, 0, 0, 255},
            {-1, 0, 255, 0, 0, 255}, {-1, -1, 255, 0, 0, 255}, {0, -1, 255, 0, 0, 255}};
        const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
        const float tints[2][3] = {{1, 0, 1}, {0, 1, 0}};  // red stays red; then green

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> vertex_buffer, upload, readback, target;
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

        // Buffers D3D12 refuses: no bytes, or a size that wraps when aligned. One the arena cannot hold, and a
        // shader-visible heap it cannot hold (512 MiB of descriptors in the 256 MiB default), run out of memory.
        {
            Ref<ID3D12Resource> bad;
            for (const UINT64 width : {UINT64(0), ~UINT64(0) - 65535}) {
                const auto bad_desc = buffer_desc(width);
                EXPECT(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &bad_desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&bad.p)) == E_INVALIDARG && !bad.p);
            }
            const auto huge = buffer_desc(UINT64(1) << 40);
            EXPECT(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &huge,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&bad.p)) == E_OUTOFMEMORY && !bad.p);
            const D3D12_DESCRIPTOR_HEAP_DESC too_many{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1u << 24,
                                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            Ref<ID3D12DescriptorHeap> big_heap;
            EXPECT(device->CreateDescriptorHeap(&too_many, IID_PPV_ARGS(&big_heap.p)) == E_OUTOFMEMORY && !big_heap.p);
        }
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = size;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target.p))));

        // Upload layout: vertices at 0, indices at 256, the tints at 512 and 768 (CBVs are 256-byte aligned).
        std::uint8_t* up = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
        std::memcpy(up, vertices, sizeof(vertices));
        std::memcpy(up + 256, indices, sizeof(indices));
        std::memcpy(up + 512, tints[0], sizeof(tints[0]));
        std::memcpy(up + 768, tints[1], sizeof(tints[1]));
        const std::uint8_t instance_colors[2][4] = {{255, 255, 0, 255}, {255, 0, 255, 255}};  // yellow, magenta
        std::memcpy(up + 1024, instance_colors, sizeof(instance_colors));
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

        // Upload the vertices, prepare the target.
        list->CopyBufferRegion(vertex_buffer.p, 0, upload.p, 0, sizeof(vertices));
        D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(vertex_buffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
            transition(target.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        list->ResourceBarrier(2, barriers);
        const FLOAT blue[4] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv, blue, 0, nullptr);

        // State for the draws; the pipeline came with CreateCommandList.
        list->SetGraphicsRootSignature(root.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), sizeof(Vertex)};
        list->IASetVertexBuffers(0, 1, &vbv);
        const D3D12_INDEX_BUFFER_VIEW ibv{upload_va + 256, sizeof(indices), DXGI_FORMAT_R16_UINT};
        list->IASetIndexBuffer(&ibv);
        const D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        // Draw 1: no offset, red tint -> top-left quadrant red. The two-target PSO with a depth format, with only
        // RTV 0 and no DSV bound: it runs as a variant, validation-clean.
        const float origin[2] = {0, 0}, down_right[2] = {1, -1};
        list->SetPipelineState(mrt.p);
        list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
        list->DrawIndexedInstanced(6, 1, 0, 0, 0);
        // Draw 2: offset (1, -1), green tint -> bottom-right quadrant green. The root signature is set again after
        // the arguments; it is the same one, so they stay bound (else the quad would land top-left, untinted).
        list->SetPipelineState(pso.p);
        list->SetGraphicsRoot32BitConstants(0, 2, down_right, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 768);
        list->SetGraphicsRootSignature(root.p);
        list->DrawIndexedInstanced(6, 1, 0, 0, 0);
        // Draw 3: the back-facing triangle in the bottom-left quadrant; culled, so that quadrant stays blue.
        list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
        list->DrawInstanced(3, 1, 4, 0);

        // Draws 4 and 5 through a viewport on the top-right quadrant, so each quad covers a 16x16 corner of it.
        const D3D12_VIEWPORT top_right{float(size / 2), 0, float(size / 2), float(size / 2), 0, 1};
        list->RSSetViewports(1, &top_right);
        list->SetPipelineState(instanced.p);
        // Draw 4: two instances, COLOR per instance with step rate 0, so both read yellow (red with the red tint);
        // a step rate of 1 would draw magenta over it.
        const D3D12_VERTEX_BUFFER_VIEW colors_view{upload_va + 1024, sizeof(instance_colors), 4};
        list->IASetVertexBuffers(1, 1, &colors_view);
        list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
        list->DrawIndexedInstanced(6, 2, 0, 0, 0);
        // Draw 5: slot 1 unbound. D3D12 reads zeros there, so COLOR is 0 and the corner is black, and the
        // submission must not fail.
        list->IASetVertexBuffers(1, 1, nullptr);
        list->SetGraphicsRoot32BitConstants(0, 2, down_right, 0);
        list->DrawIndexedInstanced(6, 1, 0, 0, 0);

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

        // Second pass on the same target, cleared blue again: the red quad XORed into the top-left quadrant, which
        // gives (255,0,255,0) where logicOp exists, and the full-screen triangle (yellow, red tint: red) through a
        // viewport on the bottom-right quadrant, with no vertex buffer in its pipeline.
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso.p)));
        D3D12_RESOURCE_BARRIER to_target = transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &to_target);
        list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        list->SetGraphicsRootSignature(root.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
        // The full-screen draw comes first, so no vertex buffer has been bound in this submission before it: a
        // pipeline that declared the stride state would break VUID-vkCmdDraw-pStrides-04913 here.
        const D3D12_VIEWPORT bottom_right{float(size / 2), float(size / 2), float(size / 2), float(size / 2), 0, 1};
        list->RSSetViewports(1, &bottom_right);
        list->SetPipelineState(fullscreen.p);
        list->DrawInstanced(3, 1, 0, 0);
        if (logic_op) {
            list->RSSetViewports(1, &viewport);
            list->SetPipelineState(xor_pso.p);
            list->IASetVertexBuffers(0, 1, &vbv);
            list->IASetIndexBuffer(&ibv);
            list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
            list->DrawIndexedInstanced(6, 1, 0, 0, 0);
        }
        list->ResourceBarrier(1, &to_copy);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        EXPECT(list->Close() == S_OK);
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 2)) && SUCCEEDED(fence->SetEventOnCompletion(2, nullptr)));
        std::memcpy(second_pixels.data(), mapped, second_pixels.size());

        // Third pass: indexed triangle-strip cuts (0xFFFF for UINT16, 0xFFFFFFFF for UINT32) and negative control
        // without a cut. Two disjoint quads (top-left and bottom-right); without the cut, bridge triangle (2, 3, 4)
        // crosses the bottom-left quadrant from (-1, 0) through (0, 0) to (1, -1).
        {
            const Vertex strip_verts[8] = {
                {-1, 1, 255, 0, 0, 255}, {0, 1, 255, 0, 0, 255}, {-1, 0, 255, 0, 0, 255}, {0, 0, 255, 0, 0, 255},
                {1, -1, 255, 0, 0, 255}, {0, -1, 255, 0, 0, 255}, {1, 0, 255, 0, 0, 255}, {0, 0, 255, 0, 0, 255},
            };
            const std::uint16_t strip_idx16[9] = {0, 1, 2, 3, 0xFFFFu, 4, 5, 6, 7};
            const std::uint32_t strip_idx32[9] = {0, 1, 2, 3, 0xFFFFFFFFu, 4, 5, 6, 7};
            const std::uint16_t strip_nocut[8] = {0, 1, 2, 3, 4, 5, 6, 7};
            void* up_ptr = nullptr;
            EXPECT(SUCCEEDED(upload->Map(0, nullptr, &up_ptr)));
            std::memcpy(static_cast<std::uint8_t*>(up_ptr) + 1024, strip_verts, sizeof(strip_verts));
            std::memcpy(static_cast<std::uint8_t*>(up_ptr) + 1280, strip_idx16, sizeof(strip_idx16));
            std::memcpy(static_cast<std::uint8_t*>(up_ptr) + 1536, strip_idx32, sizeof(strip_idx32));
            std::memcpy(static_cast<std::uint8_t*>(up_ptr) + 1792, strip_nocut, sizeof(strip_nocut));
            upload->Unmap(0, nullptr);

            auto strip_desc = pso_desc;
            strip_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            Ref<ID3D12PipelineState> strip16_pso, strip32_pso, strip_off_pso;
            strip_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&strip_desc, IID_PPV_ARGS(&strip16_pso.p))));
            strip_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&strip_desc, IID_PPV_ARGS(&strip32_pso.p))));
            strip_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&strip_desc, IID_PPV_ARGS(&strip_off_pso.p))));

            const D3D12_VERTEX_BUFFER_VIEW strip_vbv{upload_va + 1024, sizeof(strip_verts), sizeof(Vertex)};
            struct StripPass {
                ID3D12PipelineState* pso;
                D3D12_INDEX_BUFFER_VIEW ibv;
                UINT index_count;
                std::vector<std::uint8_t>* out;
            };
            const StripPass strip_passes[] = {
                {strip16_pso.p, {upload_va + 1280, sizeof(strip_idx16), DXGI_FORMAT_R16_UINT}, 9, &strip16_pixels},
                {strip32_pso.p, {upload_va + 1536, sizeof(strip_idx32), DXGI_FORMAT_R32_UINT}, 9, &strip32_pixels},
                {strip_off_pso.p, {upload_va + 1792, sizeof(strip_nocut), DXGI_FORMAT_R16_UINT}, 8, &strip_nocut_pixels},
            };
            UINT64 strip_fence = 10;
            for (const auto& sp : strip_passes) {
                EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, sp.pso)));
                list->ResourceBarrier(1, &to_target);
                list->ClearRenderTargetView(rtv, blue, 0, nullptr);
                list->SetGraphicsRootSignature(root.p);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                list->IASetVertexBuffers(0, 1, &strip_vbv);
                list->IASetIndexBuffer(&sp.ibv);
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
                list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
                list->DrawIndexedInstanced(sp.index_count, 1, 0, 0, 0);
                list->ResourceBarrier(1, &to_copy);
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                EXPECT(list->Close() == S_OK);
                queue->ExecuteCommandLists(1, lists);
                EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
                ++strip_fence;
                EXPECT(SUCCEEDED(queue->Signal(fence.p, strip_fence)) &&
                       SUCCEEDED(fence->SetEventOnCompletion(strip_fence, nullptr)));
                std::memcpy(sp.out->data(), mapped, sp.out->size());
            }
        }

        // Read-only DSV (D3D12_DSV_FLAG_READ_ONLY_DEPTH / READ_ONLY_STENCIL), depth test, and OMSetStencilRef.
        {
            D3D12_RESOURCE_DESC ds_desc = rt_desc;
            ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE ds_clear{};
            ds_clear.Format = ds_desc.Format;
            ds_clear.DepthStencil = {0.5f, 0};
            Ref<ID3D12Resource> ds_target;
            EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &ds_desc,
                                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &ds_clear,
                                                             IID_PPV_ARGS(&ds_target.p))));
            D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 3,
                                                     D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            Ref<ID3D12DescriptorHeap> dsv_heap;
            EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap.p))));
            const UINT dsv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            const auto dsv_base = reinterpret_cast<XboxHeapHandleFn>(slot(dsv_heap.p, 9))(dsv_heap.p);
            const auto dsv_slot = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(dsv_base + i * dsv_step)}; };
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
            dsv_desc.Format = ds_desc.Format;
            dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
            device->CreateDepthStencilView(ds_target.p, &dsv_desc, dsv_slot(0));
            dsv_desc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
            device->CreateDepthStencilView(ds_target.p, &dsv_desc, dsv_slot(1));
            dsv_desc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
            device->CreateDepthStencilView(ds_target.p, &dsv_desc, dsv_slot(2));

            auto ds_pso_desc = pso_desc;
            ds_pso_desc.DSVFormat = ds_desc.Format;
            ds_pso_desc.DepthStencilState.DepthEnable = TRUE;
            ds_pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            ds_pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            ds_pso_desc.DepthStencilState.StencilEnable = TRUE;
            ds_pso_desc.DepthStencilState.StencilReadMask = 0xFF;
            ds_pso_desc.DepthStencilState.StencilWriteMask = 0xFF;
            ds_pso_desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                       D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
            ds_pso_desc.DepthStencilState.BackFace = ds_pso_desc.DepthStencilState.FrontFace;
            Ref<ID3D12PipelineState> ds_write_pso, ds_ro_less_pso, ds_ro_le_pso, ds_ro_stencil_pso;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&ds_pso_desc, IID_PPV_ARGS(&ds_write_pso.p))));

            auto ro_depth_desc = pso_desc;
            ro_depth_desc.DSVFormat = ds_desc.Format;
            ro_depth_desc.DepthStencilState.DepthEnable = TRUE;
            ro_depth_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            ro_depth_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&ro_depth_desc, IID_PPV_ARGS(&ds_ro_less_pso.p))));
            ro_depth_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&ro_depth_desc, IID_PPV_ARGS(&ds_ro_le_pso.p))));

            auto ro_stencil_desc = pso_desc;
            ro_stencil_desc.DSVFormat = ds_desc.Format;
            ro_stencil_desc.DepthStencilState.StencilEnable = TRUE;
            ro_stencil_desc.DepthStencilState.StencilReadMask = 0xFF;
            ro_stencil_desc.DepthStencilState.StencilWriteMask = 0x00;
            ro_stencil_desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                           D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL};
            ro_stencil_desc.DepthStencilState.BackFace = ro_stencil_desc.DepthStencilState.FrontFace;
            EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&ro_stencil_desc, IID_PPV_ARGS(&ds_ro_stencil_pso.p))));

            const float br_offset[2] = {1.0f, -1.0f};
            const auto dsv0 = dsv_slot(0), dsv1 = dsv_slot(1), dsv2 = dsv_slot(2);
            const auto ds_to_read = transition(ds_target.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
            const auto ds_to_write = transition(ds_target.p, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            UINT64 ds_fence = 20;

            for (UINT pass = 0; pass < 2; ++pass) {
                EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, ds_write_pso.p)));
                list->ResourceBarrier(1, &to_target);
                if (pass > 0) list->ResourceBarrier(1, &ds_to_write);
                list->ClearRenderTargetView(rtv, blue, 0, nullptr);
                list->ClearDepthStencilView(dsv0, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.5f, 0, 0, nullptr);
                list->SetGraphicsRootSignature(root.p);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->IASetVertexBuffers(0, 1, &vbv);
                list->IASetIndexBuffer(&ibv);
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->OMSetRenderTargets(1, &rtv, FALSE, &dsv0);
                list->OMSetStencilRef(1);
                list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
                list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
                list->DrawIndexedInstanced(6, 1, 0, 0, 0);

                list->ResourceBarrier(1, &ds_to_read);
                list->SetPipelineState(pass == 0 ? ds_ro_less_pso.p : ds_ro_le_pso.p);
                list->OMSetRenderTargets(1, &rtv, FALSE, &dsv1);
                list->SetGraphicsRootConstantBufferView(1, upload_va + 768);
                list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
                list->DrawIndexedInstanced(6, 1, 0, 0, 0);
                list->SetGraphicsRoot32BitConstants(0, 2, br_offset, 0);
                list->DrawIndexedInstanced(6, 1, 0, 0, 0);

                list->ResourceBarrier(1, &to_copy);
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                EXPECT(list->Close() == S_OK);
                queue->ExecuteCommandLists(1, lists);
                EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
                ++ds_fence;
                EXPECT(SUCCEEDED(queue->Signal(fence.p, ds_fence)) &&
                       SUCCEEDED(fence->SetEventOnCompletion(ds_fence, nullptr)));
                auto& out = pass == 0 ? ro_depth_less : ro_depth_le;
                std::memcpy(out.data(), mapped, out.size());
            }

            for (UINT pass = 0; pass < 2; ++pass) {
                EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, ds_ro_stencil_pso.p)));
                list->ResourceBarrier(1, &to_target);
                list->ClearRenderTargetView(rtv, blue, 0, nullptr);
                list->SetGraphicsRootSignature(root.p);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->IASetVertexBuffers(0, 1, &vbv);
                list->IASetIndexBuffer(&ibv);
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->OMSetRenderTargets(1, &rtv, FALSE, &dsv2);
                list->OMSetStencilRef(pass == 0 ? 1u : 2u);
                list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
                list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
                list->DrawIndexedInstanced(6, 1, 0, 0, 0);
                list->SetGraphicsRoot32BitConstants(0, 2, br_offset, 0);
                list->DrawIndexedInstanced(6, 1, 0, 0, 0);
                list->ResourceBarrier(1, &to_copy);
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                EXPECT(list->Close() == S_OK);
                queue->ExecuteCommandLists(1, lists);
                EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
                ++ds_fence;
                EXPECT(SUCCEEDED(queue->Signal(fence.p, ds_fence)) &&
                       SUCCEEDED(fence->SetEventOnCompletion(ds_fence, nullptr)));
                auto& out = pass == 0 ? ro_stencil_eq1 : ro_stencil_eq2;
                std::memcpy(out.data(), mapped, out.size());
            }
        }

        // A draw with no viewport (never set, or unbound with a count of 0) rasterizes nothing in D3D12. It is not
        // recorded, so the list closes and executes cleanly (Vulkan forbids a 0x0 viewport).
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso.p)));
        list->SetGraphicsRootSignature(root.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        list->RSSetViewports(1, &viewport);
        list->RSSetViewports(0, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        EXPECT(list->Close() == S_OK);
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 30)) && SUCCEEDED(fence->SetEventOnCompletion(30, nullptr)));

        // A draw without a topology is invalid; Close reports it.
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso.p)));
        list->SetGraphicsRootSignature(root.p);
        list->DrawInstanced(3, 1, 0, 0);
        EXPECT(list->Close() == E_INVALIDARG);
        // So is binding more than eight render targets.
        EXPECT(SUCCEEDED(list->Reset(allocator.p, pso.p)));
        list->OMSetRenderTargets(9, &rtv, TRUE, nullptr);
        EXPECT(list->Close() == E_INVALIDARG);

        for (auto* r : {&vertex_buffer, &upload, &readback, &target}) r->reset();
        for (auto* p : {&mrt, &instanced, &refused, &unfed, &xor_pso, &fullscreen}) p->reset();
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
            const bool top = row < size / 2, left = col < size / 2;
            // In the top-right quadrant: draw 4's red corner (top-left of it) and draw 5's black one (bottom-right).
            const bool corner4 = row < size / 4 && col >= size / 2 && col < size * 3 / 4;
            const bool corner5 = top && row >= size / 4 && col >= size * 3 / 4;
            using Pixel = std::array<std::uint8_t, 4>;
            const Pixel want = (top && left) || corner4 ? Pixel{255, 0, 0, 255}
                               : corner5                ? Pixel{0, 0, 0, 255}
                               : !top && !left          ? Pixel{0, 255, 0, 255}
                                                        : Pixel{0, 0, 255, 255};
            const std::uint8_t* got = pixels.data() + row * 256 + col * 4;
            if (std::equal(want.begin(), want.end(), got)) continue;
            if (mismatches++ < 6)
                std::fprintf(stderr, "pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row, got[0], got[1],
                             got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    }
    EXPECT(!mismatches);
    // Second pass: XOR quad top-left (where logicOp exists), full-screen triangle bottom-right, blue elsewhere.
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const bool top = row < size / 2, left = col < size / 2;
            using Pixel = std::array<std::uint8_t, 4>;
            const Pixel want = top && left && logic_op ? Pixel{255, 0, 255, 0}
                               : !top && !left         ? Pixel{255, 0, 0, 255}
                                                       : Pixel{0, 0, 255, 255};
            const std::uint8_t* got = second_pixels.data() + row * 256 + col * 4;
            if (std::equal(want.begin(), want.end(), got)) continue;
            if (mismatches++ < 6)
                std::fprintf(stderr, "second pass pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row,
                             got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    }
    EXPECT(!mismatches);
    // Strip-cut passes: 0xFFFF (UINT16) and 0xFFFFFFFF (UINT32) must produce only the top-left and bottom-right quads
    // (pure blue in top-right and bottom-left). Negative control without cut must paint bridge pixels in bottom-left.
    UINT nocut_bl_red = 0;
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const bool top = row < size / 2, left = col < size / 2;
            using Pixel = std::array<std::uint8_t, 4>;
            const Pixel want = (top && left) || (!top && !left) ? Pixel{255, 0, 0, 255} : Pixel{0, 0, 255, 255};
            for (const auto& buf : {strip16_pixels, strip32_pixels}) {
                const std::uint8_t* got = buf.data() + row * 256 + col * 4;
                if (!std::equal(want.begin(), want.end(), got) && mismatches++ < 6)
                    std::fprintf(stderr, "strip-cut pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row,
                                 got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
            }
            if (!top && left) {
                const std::uint8_t* got = strip_nocut_pixels.data() + row * 256 + col * 4;
                if (got[0] == 255 && got[2] == 0) ++nocut_bl_red;
            }
        }
    }
    EXPECT(!mismatches);
    EXPECT(nocut_bl_red > 0);
    std::printf("strip cut (0xFFFF & 0xFFFFFFFF): 4096/4096 pixels matched each (no-cut negative control painted %u "
                "bridge pixels in bottom-left)\n",
                nocut_bl_red);
    UINT depth_le_flipped = 0, stencil_ref2_flipped = 0;
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const bool top = row < size / 2, left = col < size / 2;
            using Pixel = std::array<std::uint8_t, 4>;
            const Pixel want_less = top && left     ? Pixel{255, 0, 0, 255}
                                    : !top && !left ? Pixel{0, 255, 0, 255}
                                                    : Pixel{0, 0, 255, 255};
            const Pixel want_le = (top && left) || (!top && !left) ? Pixel{0, 255, 0, 255} : Pixel{0, 0, 255, 255};
            const Pixel want_st1 = top && left ? Pixel{255, 0, 0, 255} : Pixel{0, 0, 255, 255};
            const Pixel want_st2 = {0, 0, 255, 255};
            const std::uint8_t* g_less = ro_depth_less.data() + row * 256 + col * 4;
            const std::uint8_t* g_le = ro_depth_le.data() + row * 256 + col * 4;
            const std::uint8_t* g_st1 = ro_stencil_eq1.data() + row * 256 + col * 4;
            const std::uint8_t* g_st2 = ro_stencil_eq2.data() + row * 256 + col * 4;
            if (!std::equal(want_less.begin(), want_less.end(), g_less) && mismatches++ < 6)
                std::fprintf(stderr, "ro-depth LESS (%u,%u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row, g_less[0],
                             g_less[1], g_less[2], g_less[3], want_less[0], want_less[1], want_less[2], want_less[3]);
            if (!std::equal(want_le.begin(), want_le.end(), g_le) && mismatches++ < 6)
                std::fprintf(stderr, "ro-depth LESS_EQUAL (%u,%u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row,
                             g_le[0], g_le[1], g_le[2], g_le[3], want_le[0], want_le[1], want_le[2], want_le[3]);
            if (!std::equal(want_st1.begin(), want_st1.end(), g_st1) && mismatches++ < 6)
                std::fprintf(stderr, "ro-stencil ref=1 (%u,%u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row, g_st1[0],
                             g_st1[1], g_st1[2], g_st1[3], want_st1[0], want_st1[1], want_st1[2], want_st1[3]);
            if (!std::equal(want_st2.begin(), want_st2.end(), g_st2) && mismatches++ < 6)
                std::fprintf(stderr, "ro-stencil ref=2 (%u,%u): got %u,%u,%u,%u want 0,0,255,255\n", col, row, g_st2[0],
                             g_st2[1], g_st2[2], g_st2[3]);
            if (top && left && std::memcmp(g_less, g_le, 4) != 0) ++depth_le_flipped;
            if (top && left && std::memcmp(g_st1, g_st2, 4) != 0) ++stencil_ref2_flipped;
        }
    }
    EXPECT(!mismatches);
    EXPECT(depth_le_flipped == (size / 2) * (size / 2) && stencil_ref2_flipped == (size / 2) * (size / 2));
    std::printf("read-only DSV (depth & stencil): 4096/4096 pixels matched across 4 passes (LESS_EQUAL flipped %u/%u, "
                "stencil ref=2 flipped %u/%u top-left pixels)\n",
                depth_le_flipped, (size / 2) * (size / 2), stencil_ref2_flipped, (size / 2) * (size / 2));
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("M1: five draws and a culled triangle matched all %u pixels: input layout by semantic, root "
                "constants and a root CBV per draw, vertex buffer in the arena, index buffer in an upload heap, an "
                "unbound target and DSV, a rebound root signature, step rate 0 and an unbound vertex slot; then %s "
                "and a full-screen pass without vertex input\n",
                size * size, logic_op ? "an XOR logic op" : "(no logicOp on this GPU)");
    return 0;
}
