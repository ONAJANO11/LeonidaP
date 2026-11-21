// Variable rate shading (D3D-32) through the Xbox-ABI proxy over the Vulkan device, twice: on a Vulkan device with
// VK_KHR_fragment_shading_rate, and on one without it.
//   - D3D12_OPTIONS6 and OPTIONS10 are compared with the host PC D3D12 runtime's answers for the same adapter
//     (matched by LUID), where it has one; without the extension they report no VRS;
//   - a triangle covering a 64x64 target runs a pixel shader that writes the pixel it ran at (test/glsl/ps_vrs.frag).
//     A coarse pixel runs once for all the pixels it covers, so the image read back shows the rate as runs of equal
//     values. The cases: 1x1 by default and after Reset and ClearState, each per-draw rate, the first combiner
//     against the 1x1 per-primitive rate, a shading-rate image of four quadrants with different rates under each
//     second combiner, an unbound image, and a pixel shader that writes depth, which D3D12 shades per pixel;
//   - without the extension, the rate and the image are ignored and every pixel is shaded;
//   - a rate outside D3D12_SHADING_RATE is an invalid call, reported at Close.
// A device with less than 144 bytes of push constants refuses every PSO until #224's uniform-buffer tail exists;
// there only the feature answers are checked, and the test says so.
// The shaders are GLSL written to the translator's contract; no game data. LEONIDA_VK_VALIDATION=1 runs the test
// under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"

#include <dxgi1_4.h>

#include <cstring>
#include <functional>

#if !defined(LEONIDA_TEST_VRS_VS_SPV) || !defined(LEONIDA_TEST_VRS_PS_SPV) || !defined(LEONIDA_TEST_VRS_PS_DEPTH_SPV)
#error "LEONIDA_TEST_VRS_VS_SPV, LEONIDA_TEST_VRS_PS_SPV and LEONIDA_TEST_VRS_PS_DEPTH_SPV must name the test shaders"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

namespace {

constexpr UINT kSize = 64;      // the render target, in pixels
constexpr UINT kPitch = 256;    // readback and upload row pitch
constexpr UINT kHalf = kSize / 2;

bool draws_refused = false;  // the device's push-constant limit refused every PSO (see run)

struct Size {
    UINT w, h;  // the coarse pixel; {0, 0}: not checked
};

// The rate a D3D12_SHADING_RATE gives on a GPU with or without the additional rates (2x4, 4x2, 4x4): D3D12 clamps
// each axis to 2 without them.
Size size_of(D3D12_SHADING_RATE rate, bool additional) {
    const UINT limit = additional ? 4 : 2;
    return {std::min(1u << (UINT(rate) >> 2), limit), std::min(1u << (UINT(rate) & 3), limit)};
}

// The shading-rate image's quadrants: top-left 1x1, top-right 2x2, bottom-left 2x1, bottom-right 1x2.
D3D12_SHADING_RATE quadrant_rate(UINT x, UINT y) {
    if (y < kHalf) return x < kHalf ? D3D12_SHADING_RATE_1X1 : D3D12_SHADING_RATE_2X2;
    return x < kHalf ? D3D12_SHADING_RATE_2X1 : D3D12_SHADING_RATE_1X2;
}

using PFN_CreateFactory = HRESULT(WINAPI*)(REFIID, void**);

// The host PC D3D12 runtime's answers for the adapter with this LUID; false when it has no such adapter.
bool host_options(const LUID& luid, D3D12_FEATURE_DATA_D3D12_OPTIONS6& options6,
                  D3D12_FEATURE_DATA_D3D12_OPTIONS10& options10) {
    const HMODULE dxgi = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const HMODULE d3d12 = LoadLibraryExA("d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create_factory = dxgi ? reinterpret_cast<PFN_CreateFactory>(
                                           reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                                     : nullptr;
    const auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
                                           reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")))
                                     : nullptr;
    if (!create_factory || !create_device) return false;
    Ref<IDXGIFactory4> factory;
    Ref<IDXGIAdapter1> adapter;
    Ref<ID3D12Device> device;
    if (FAILED(create_factory(IID_PPV_ARGS(&factory.p))) ||
        FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter.p))) ||
        FAILED(create_device(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device.p))))
        return false;
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6))) &&
           SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS10, &options10, sizeof(options10)));
}

// One run on a Vulkan device with (shading_rate) or without VK_KHR_fragment_shading_rate.
int run(const std::filesystem::path& library, const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps,
        const std::vector<std::uint8_t>& ps_depth, bool shading_rate) {
    vktest::Device d;
    EXPECT(d.create(false, shading_rate));
    std::printf("%s VK_KHR_fragment_shading_rate (pipeline %d, attachment %d)\n", shading_rate ? "with" : "without",
                d.fragment_shading_rate, d.attachment_fragment_shading_rate);
    if (shading_rate && !d.fragment_shading_rate) {
        std::printf("the GPU has no pipelineFragmentShadingRate: nothing more to test here\n");
        return 0;
    }

    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.buffer_device_address = true;
    info.fragment_shading_rate = d.fragment_shading_rate;
    info.attachment_fragment_shading_rate = d.attachment_fragment_shading_rate;
    info.shader_dir = library.string();
    ID3D12Device* inner = nullptr;
    EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
    Ref<ID3D12Device> device;
    EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                       reinterpret_cast<void**>(&device.p))));
    rt::d3d12::set_device_outer(inner, device.p);

    // Feature answers.
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS10 options10{};
    EXPECT(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6) - 1) == E_INVALIDARG);
    EXPECT(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6))));
    EXPECT(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS10, &options10, sizeof(options10))));
    std::printf("OPTIONS6: tier %d, tile %u, additional rates %d; OPTIONS10: sum combiner %d\n",
                int(options6.VariableShadingRateTier), options6.ShadingRateImageTileSize,
                options6.AdditionalShadingRatesSupported, options10.VariableRateShadingSumCombinerSupported);
    EXPECT(!options6.PerPrimitiveShadingRateSupportedWithViewportIndexing && !options6.BackgroundProcessingSupported);
    EXPECT(!options10.MeshShaderPerPrimitiveShadingRateSupported);
    const D3D12_VARIABLE_SHADING_RATE_TIER tier = options6.VariableShadingRateTier;
    const UINT tile = options6.ShadingRateImageTileSize;
    const bool additional = options6.AdditionalShadingRatesSupported;
    if (!shading_rate) {
        EXPECT(tier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED && !tile && !additional &&
               !options10.VariableRateShadingSumCombinerSupported);
    } else {
        EXPECT(tier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED);
        EXPECT(tier == D3D12_VARIABLE_SHADING_RATE_TIER_2 ? tile == 8 || tile == 16 : tile == 0);
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        d.instance_fn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(d.physical, &properties);
        LUID luid{};
        std::memcpy(&luid, id.deviceLUID, sizeof(luid));
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 host6{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS10 host10{};
        if (id.deviceLUIDValid && host_options(luid, host6, host10)) {
            std::printf("host D3D12 on the same adapter: tier %d, tile %u, additional rates %d, sum combiner %d\n",
                        int(host6.VariableShadingRateTier), host6.ShadingRateImageTileSize,
                        host6.AdditionalShadingRatesSupported, host10.VariableRateShadingSumCombinerSupported);
            EXPECT(tier == host6.VariableShadingRateTier && tile == host6.ShadingRateImageTileSize &&
                   additional == bool(host6.AdditionalShadingRatesSupported));
            EXPECT(bool(options10.VariableRateShadingSumCombinerSupported) ==
                   bool(host10.VariableRateShadingSumCombinerSupported));
        } else {
            std::printf("no host D3D12 device on this adapter: feature answers not compared\n");
        }
    }

    // An empty root signature, and a pipeline per pixel shader. No input layout: the VS reads SV_VertexID only.
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    const auto root_blob = serialize_root_signature(root_desc);
    EXPECT(!root_blob.empty());
    Ref<ID3D12RootSignature> root;
    EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.p;
    pso_desc.VS = {vs.data(), vs.size()};
    pso_desc.PS = {ps.data(), ps.size()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = 0xffffffff;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    Ref<ID3D12PipelineState> pso, pso_depth;
    const HRESULT created = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso.p));
    if (rt::d3d12::device_push_constants_need_fallback(device.p)) {
        // Below 144 bytes of push constants every PSO is refused until #224's uniform-buffer tail exists, so this
        // device answers the feature queries but draws nothing.
        EXPECT(FAILED(created));
        std::printf("maxPushConstantsSize %u: PSOs are refused below 144 bytes (#224), so no draws on this device\n",
                    rt::d3d12::device_max_push_constants_size(device.p));
        draws_refused = true;
        root.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
        return 0;
    }
    EXPECT(SUCCEEDED(created));
    pso_desc.PS = {ps_depth.data(), ps_depth.size()};
    EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_depth.p))));

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> target, readback, upload, rate_image;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = rt_desc.Height = kSize;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                     IID_PPV_ARGS(&target.p))));
    const auto readback_desc = buffer_desc(kSize * kPitch);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback.p))));
    const auto upload_desc = buffer_desc(kSize * kPitch);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&upload.p))));
    // The shading-rate image: one R8_UINT texel per tile (an 8x8 tile when the device has no image support, which
    // the layer must then ignore).
    const UINT texel = tile ? tile : 8;
    const UINT image_w = (kSize + texel - 1) / texel, image_h = (kSize + texel - 1) / texel;
    D3D12_RESOURCE_DESC image_desc = rt_desc;
    image_desc.Width = image_w;
    image_desc.Height = image_h;
    image_desc.Format = DXGI_FORMAT_R8_UINT;
    image_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &image_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&rate_image.p))));
    std::uint8_t* up = nullptr;
    EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
    for (UINT y = 0; y < image_h; ++y)
        for (UINT x = 0; x < image_w; ++x) up[y * kPitch + x] = std::uint8_t(quadrant_rate(x * texel, y * texel));
    void* mapped = nullptr;
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    const auto* down = static_cast<const std::uint8_t*>(mapped);

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    Ref<ID3D12DescriptorHeap> rtv_heap;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p))};
    device->CreateRenderTargetView(target.p, nullptr, rtv);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12GraphicsCommandList5> list5;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso.p,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list5.p))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    UINT64 fence_value = 0;
    const auto submit = [&]() {
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        return rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success &&
               SUCCEEDED(queue->Signal(fence.p, ++fence_value)) &&
               SUCCEEDED(fence->SetEventOnCompletion(fence_value, nullptr));
    };

    // Upload the image (the list came open from CreateCommandList).
    D3D12_TEXTURE_COPY_LOCATION image_dst{};
    image_dst.pResource = rate_image.p;
    image_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION image_src{};
    image_src.pResource = upload.p;
    image_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    image_src.PlacedFootprint = {0, {DXGI_FORMAT_R8_UINT, image_w, image_h, 1, kPitch}};
    list->CopyTextureRegion(&image_dst, 0, 0, 0, &image_src, nullptr);
    const D3D12_RESOURCE_BARRIER to_source =
        transition(rate_image.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
    list->ResourceBarrier(1, &to_source);
    EXPECT(list->Close() == S_OK);
    EXPECT(submit());

    // One draw over the whole target. `first` runs right after Reset; `vrs` right before the draw.
    using Step = std::function<void(ID3D12GraphicsCommandList5*)>;
    const D3D12_VIEWPORT viewport{0, 0, float(kSize), float(kSize), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(kSize), LONG(kSize)};
    const auto render = [&](ID3D12PipelineState* state, const Step& first, const Step& vrs) {
        if (FAILED(list->Reset(allocator.p, state))) return false;
        if (first) first(list5.p);
        list->SetGraphicsRootSignature(root.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const FLOAT unset[4] = {1, 1, 1, 0};  // alpha 0: not drawn
        list->ClearRenderTargetView(rtv, unset, 0, nullptr);
        if (vrs) vrs(list5.p);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier =
            transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kPitch}};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier = transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &barrier);
        return list->Close() == S_OK && submit();
    };

    // Each pixel holds the pixel its coarse pixel ran at. For an expected w x h coarse pixel, every pixel matches
    // the first pixel of its aligned w x h block (not finer) and that position lies inside the block (not coarser).
    unsigned failures = 0;
    const auto check = [&](const char* name, const std::function<Size(UINT, UINT)>& expected) {
        unsigned bad = 0;
        for (UINT y = 0; y < kSize; ++y) {
            for (UINT x = 0; x < kSize; ++x) {
                const Size s = expected(x, y);
                if (!s.w) continue;
                const UINT x0 = x - x % s.w, y0 = y - y % s.h;
                const std::uint8_t* p = down + y * kPitch + x * 4;
                const std::uint8_t* o = down + y0 * kPitch + x0 * 4;
                if (p[3] == 255 && p[0] == o[0] && p[1] == o[1] && p[0] >= x0 && p[0] < x0 + s.w && p[1] >= y0 &&
                    p[1] < y0 + s.h)
                    continue;
                if (bad++ < 4)
                    std::fprintf(stderr, "%s: pixel (%u, %u) ran at (%u, %u), alpha %u; want a %ux%u coarse pixel "
                                 "from (%u, %u)\n", name, x, y, p[0], p[1], p[3], s.w, s.h, x0, y0);
            }
        }
        std::printf("  %-58s %s\n", name, bad ? "FAILED" : "ok");
        if (bad) ++failures;
    };
    const auto everywhere = [](Size s) { return [s](UINT, UINT) { return s; }; };
    const auto rate = [](D3D12_SHADING_RATE r, D3D12_SHADING_RATE_COMBINER c0 = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                         D3D12_SHADING_RATE_COMBINER c1 = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH) {
        return [=](ID3D12GraphicsCommandList5* l) {
            const D3D12_SHADING_RATE_COMBINER combiners[2] = {c0, c1};
            l->RSSetShadingRate(r, combiners);
        };
    };
    const Size fine{1, 1};

    EXPECT(render(pso.p, nullptr, nullptr));
    check("default: 1x1", everywhere(fine));

    if (!shading_rate) {
        // No VRS on the device: the rate and the image are ignored.
        EXPECT(render(pso.p, nullptr, rate(D3D12_SHADING_RATE_2X2)));
        check("2x2 without VK_KHR_fragment_shading_rate: 1x1", everywhere(fine));
        EXPECT(render(pso.p, nullptr, [&](ID3D12GraphicsCommandList5* l) {
            rate(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                 D3D12_SHADING_RATE_COMBINER_OVERRIDE)(l);
            l->RSSetShadingRateImage(rate_image.p);
        }));
        check("image without VK_KHR_fragment_shading_rate: 1x1", everywhere(fine));
    } else {
        for (const D3D12_SHADING_RATE r : {D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_2X1, D3D12_SHADING_RATE_2X2,
                                           D3D12_SHADING_RATE_2X4, D3D12_SHADING_RATE_4X2, D3D12_SHADING_RATE_4X4}) {
            EXPECT(render(pso.p, nullptr, rate(r)));
            const Size s = size_of(r, additional);
            char name[64];
            std::snprintf(name, sizeof(name), "RSSetShadingRate 0x%x: %ux%u", unsigned(r), s.w, s.h);
            check(name, everywhere(s));
        }
        EXPECT(render(pso.p, nullptr, nullptr));
        check("after Reset: 1x1", everywhere(fine));
        EXPECT(render(pso.p, nullptr, [&](ID3D12GraphicsCommandList5* l) { l->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr); }));
        check("2x2, null combiners (passthrough): 2x2", everywhere({2, 2}));
        // The per-primitive rate is 1x1 (the shader does not write SV_ShadingRate).
        EXPECT(render(pso.p, nullptr, rate(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER_OVERRIDE)));
        check("2x2, override by the 1x1 per-primitive rate: 1x1", everywhere(fine));
        EXPECT(render(pso.p, nullptr, rate(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER_MIN)));
        check("2x2, min with the per-primitive rate: 1x1", everywhere(fine));
        EXPECT(render(pso.p, nullptr, rate(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER_MAX)));
        check("2x2, max with the per-primitive rate: 2x2", everywhere({2, 2}));
        EXPECT(render(pso.p, nullptr, rate(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER_SUM)));
        check("2x2, sum with the per-primitive rate: 2x2", everywhere({2, 2}));
        EXPECT(render(pso.p, nullptr,
                      rate(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER(9), D3D12_SHADING_RATE_COMBINER(9))));
        check("2x2, combiners outside the enum (passthrough): 2x2", everywhere({2, 2}));
        // No image bound: the image's rate is 1x1.
        EXPECT(render(pso.p, nullptr,
                      rate(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                           D3D12_SHADING_RATE_COMBINER_OVERRIDE)));
        check("2x2, override by no image: 1x1", everywhere(fine));
        EXPECT(render(pso.p, [&](ID3D12GraphicsCommandList5* l) {
            l->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
            l->ClearState(pso.p);
        }, nullptr));
        check("2x2, then ClearState: 1x1", everywhere(fine));
        EXPECT(render(pso_depth.p, nullptr, rate(D3D12_SHADING_RATE_2X2)));
        check("2x2, pixel shader writes depth: 1x1", everywhere(fine));

        if (tile) {
            const auto with_image = [&](D3D12_SHADING_RATE r, D3D12_SHADING_RATE_COMBINER c1) {
                return [&, r, c1](ID3D12GraphicsCommandList5* l) {
                    rate(r, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, c1)(l);
                    l->RSSetShadingRateImage(rate_image.p);
                };
            };
            const auto quadrants = [&](UINT x, UINT y) { return size_of(quadrant_rate(x, y), additional); };
            EXPECT(render(pso.p, nullptr, with_image(D3D12_SHADING_RATE_4X4, D3D12_SHADING_RATE_COMBINER_OVERRIDE)));
            check("image, override: the quadrants' rates", quadrants);
            EXPECT(render(pso.p, nullptr, with_image(D3D12_SHADING_RATE_2X1, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH)));
            check("image, passthrough of 2x1: 2x1", everywhere({2, 1}));
            EXPECT(render(pso.p, nullptr, with_image(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER_MIN)));
            check("image, min with 2x2: the quadrants' rates", quadrants);
            EXPECT(render(pso.p, nullptr, with_image(D3D12_SHADING_RATE_2X1, D3D12_SHADING_RATE_COMBINER_MAX)));
            check("image, max with 2x1: 2x1, 2x2 / 2x1, 2x2", [](UINT x, UINT) { return x < kHalf ? Size{2, 1} : Size{2, 2}; });
            if (options10.VariableRateShadingSumCombinerSupported) {
                // 1x2 + 1x1 = 1x2; 1x2 + 2x2 = 2x4 (2x2 without the additional rates); 1x2 + 2x1 = 2x2. The
                // bottom-right 1x2 + 1x2 = 1x4 is no D3D12 rate, and D3D12 leaves what it becomes open.
                EXPECT(render(pso.p, nullptr, with_image(D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_COMBINER_SUM)));
                check("image, sum with 1x2", [&](UINT x, UINT y) {
                    if (y < kHalf) return x < kHalf ? Size{1, 2} : size_of(D3D12_SHADING_RATE_2X4, additional);
                    return x < kHalf ? Size{2, 2} : Size{0, 0};
                });
            }
            EXPECT(render(pso.p, nullptr, [&](ID3D12GraphicsCommandList5* l) {
                with_image(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_COMBINER_OVERRIDE)(l);
                l->RSSetShadingRateImage(nullptr);
            }));
            check("image unbound, override: 1x1", everywhere(fine));
            EXPECT(render(pso_depth.p, nullptr, with_image(D3D12_SHADING_RATE_4X4, D3D12_SHADING_RATE_COMBINER_OVERRIDE)));
            check("image, override, pixel shader writes depth: 1x1", everywhere(fine));
            EXPECT(render(pso.p, [&](ID3D12GraphicsCommandList5* l) {
                with_image(D3D12_SHADING_RATE_4X4, D3D12_SHADING_RATE_COMBINER_OVERRIDE)(l);
                l->ClearState(pso.p);
            }, nullptr));
            check("image, override, then ClearState: 1x1", everywhere(fine));
        }
    }

    // A rate outside D3D12_SHADING_RATE is an invalid call.
    EXPECT(SUCCEEDED(list->Reset(allocator.p, pso.p)));
    list5->RSSetShadingRate(D3D12_SHADING_RATE(3), nullptr);
    EXPECT(list->Close() == E_INVALIDARG);

    for (auto* r : {&target, &readback, &upload, &rate_image}) r->reset();
    pso.reset();
    pso_depth.reset();
    root.reset();
    rtv_heap.reset();
    list5.reset();
    list.reset();
    allocator.reset();
    queue.reset();
    fence.reset();
    EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    device.reset();
    EXPECT(!failures);
    return 0;
}

}  // namespace

int main() {
    // Shader programs and the index that maps their hashes to the compiled test modules.
    const auto vs_program = fssm_program({0x56530002, 0xbf810000});
    const auto ps_program = fssm_program({0x50530002, 0xbf810000});
    const auto ps_depth_program = fssm_program({0x50530003, 0xbf810000});
    const auto library = std::filesystem::temp_directory_path() / "leonida_vrs_test";
    std::filesystem::create_directories(library);
    const auto copy = std::filesystem::copy_options::overwrite_existing;
    std::filesystem::copy_file(LEONIDA_TEST_VRS_VS_SPV, library / "vs_vrs.spv", copy);
    std::filesystem::copy_file(LEONIDA_TEST_VRS_PS_SPV, library / "ps_vrs.spv", copy);
    std::filesystem::copy_file(LEONIDA_TEST_VRS_PS_DEPTH_SPV, library / "ps_vrs_depth.spv", copy);
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"vs_vrs\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": []},\n"
                 " \"ps_vrs\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]},\n"
                 " \"ps_vrs_depth\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_depth_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }
    const auto vs = shader_container(vs_program), ps = shader_container(ps_program);
    const auto ps_depth = shader_container(ps_depth_program);

    int result = 0;
    for (const bool shading_rate : {true, false}) {
        if ((result = run(library, vs, ps, ps_depth, shading_rate)) != 0) break;
    }
    std::filesystem::remove_all(library);
    if (result) return result;
    if (vktest::validation_error_count()) {
        std::fprintf(stderr, "%u validation errors\n", vktest::validation_error_count());
        return 1;
    }
    if (draws_refused) {
        std::printf("D3D-32: feature answers matched; no draws, because this device refuses PSOs (#224)\n");
        return 0;
    }
    std::printf("D3D-32: variable rate shading matched on every pixel of every case\n");
    return 0;
}
