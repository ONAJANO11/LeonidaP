// Output depth against the viewport's depth range, on the layer and on Windows D3D12 (default adapter, or the one
// LEONIDA_HOST_ADAPTER names, such as warp). D3D12 clamps a pixel's final depth (SV_Depth, or the interpolated depth
// after bias) to the viewport's MinDepth..MaxDepth whether DepthClipEnable is TRUE or FALSE; the flag only decides
// whether primitives are clipped against the near and far planes. Every case draws one triangle that covers a 16x16
// D32_FLOAT target (cleared to 0.75, depth test ALWAYS, depth write on, no colour target) through a viewport of
// MinDepth 0.25, MaxDepth 0.5, and reads the depth back. Both sides must return the same floats, and the host's
// numbers must be the ones in the table below (measured on an NVIDIA GeForce RTX 4060 Ti and on WARP).
// The host shaders are HLSL compiled at run time with d3dcompiler_47.dll. The layer's are GLSL written to the
// translator's contract (test/glsl/vs_depth_clamp.vert, ps_depth_clamp.frag); the root constants s0 and s1 carry the
// triangle's clip-space depth and the depth the pixel shader writes.
// Not covered: conservative depth (SV_DepthGreaterEqual / SV_DepthLessEqual) and a slope-scaled bias, which this
// triangle, of constant depth, has no slope for.
// A second pass runs the layer as if VK_EXT_depth_clip_enable were absent, to pin the documented fallback. Under Wine
// only the layer is checked, against the table. Skips (exit 77) without a Vulkan GPU, or without
// VK_EXT_depth_clip_enable and depthClamp on it; on Windows a missing host device or d3dcompiler_47.dll fails. Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "d3d12_copy_compare.h"
#include "host_wine.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../src/shader_library.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>
#include <cstring>

#if !defined(LEONIDA_TEST_VS_SPV) || !defined(LEONIDA_TEST_PS_SPV)
#error "LEONIDA_TEST_VS_SPV and LEONIDA_TEST_PS_SPV must name the compiled test shaders"
#endif

namespace {

using copycmp::Ptr;
using d3d12test::buffer_desc;
using d3d12test::transition;

constexpr UINT target_size = 16;
constexpr float initial_depth = 0.75f, min_depth = 0.25f, max_depth = 0.5f;

struct Case {
    const char* name;
    float z;             // the triangle's clip-space depth (w = 1)
    bool shader_depth;   // the pixel shader writes SV_Depth
    float depth;         // ... this value
    int bias;            // D3D12_RASTERIZER_DESC::DepthBias
    float want_clipped;  // the depth buffer after the draw with DepthClipEnable = TRUE
    float want_unclipped;  // ... and with FALSE
    int fallback_side = 0;  // without VK_EXT_depth_clip_enable and with TRUE: +1 above the range, -1 below, 0 as above
};

// A bias unit is 2^(exponent - 23) for the primitive's largest z, as a float depth buffer takes it: 2^-25 for 0.375,
// so 8,000,000 units move it by about 0.24, past either end of the range (0.125 from 0.5, 0.125 from 0.25).
const Case cases[] = {
    {"SV_Depth 0.9 above the range", 0.5f, true, 0.9f, 0, 0.5f, 0.5f, +1},
    {"SV_Depth 0.1 below the range", 0.5f, true, 0.1f, 0, 0.25f, 0.25f, -1},
    {"SV_Depth 0.4 inside the range", 0.5f, true, 0.4f, 0, 0.4f, 0.4f},
    {"no SV_Depth, z 0.5 (0.375)", 0.5f, false, 0.0f, 0, 0.375f, 0.375f},
    {"bias pushes depth past the max", 0.5f, false, 0.0f, 8000000, 0.5f, 0.5f, +1},
    {"bias pushes depth below the min", 0.5f, false, 0.0f, -8000000, 0.25f, 0.25f, -1},
    {"z/w 1.5 beyond the far plane", 1.5f, false, 0.0f, 0, initial_depth, 0.5f},
    {"z/w -0.5 before the near plane", -0.5f, false, 0.0f, 0, initial_depth, 0.25f},
};

// The compiled shaders of one side: a vertex shader, and the pixel shader that writes SV_Depth (the cases without it
// have no pixel shader at all).
struct Shaders {
    D3D12_SHADER_BYTECODE vs{}, ps{};
};

bool wait(ID3D12Device* device, ID3D12CommandQueue* queue) {
    Ptr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p)))) return false;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool done = event && SUCCEEDED(queue->Signal(fence.p, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
                      WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    if (event) CloseHandle(event);
    return done;
}

// Draws `c` with the given DepthClipEnable and reads the target back. False, with a message, when a step fails.
bool draw_and_read(ID3D12Device* device, const Shaders& shaders, const std::vector<std::uint8_t>& root_blob,
                   const Case& c, bool clip, std::vector<float>& out, float near_depth = min_depth,
                   float far_depth = max_depth) {
#define STEP(expression) do { \
    if (FAILED(expression)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #expression); return false; } \
} while (0)
    Ptr<ID3D12RootSignature> root;
    STEP(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.p;
    pso_desc.VS = shaders.vs;
    if (c.shader_depth) pso_desc.PS = shaders.ps;
    pso_desc.SampleMask = 0xffffffff;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = clip;
    pso_desc.RasterizerState.DepthBias = c.bias;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso_desc.SampleDesc.Count = 1;
    Ptr<ID3D12PipelineState> pso;
    STEP(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso.p)));

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = texture.Height = target_size;
    texture.DepthOrArraySize = texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_D32_FLOAT;
    texture.SampleDesc.Count = 1;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    Ptr<ID3D12Resource> target, readback;
    STEP(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                         nullptr, IID_PPV_ARGS(&target.p)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    const auto readback_desc = buffer_desc(total);
    STEP(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&readback.p)));

    Ptr<ID3D12DescriptorHeap> dsv_heap;
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    STEP(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap.p)));
    const auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(target.p, nullptr, dsv);

    Ptr<ID3D12CommandQueue> queue;
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> list;
    const D3D12_COMMAND_QUEUE_DESC queue_desc{};
    STEP(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p)));
    STEP(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p)));
    STEP(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso.p, IID_PPV_ARGS(&list.p)));
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, initial_depth, 0, 0, nullptr);
    list->SetGraphicsRootSignature(root.p);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, float(target_size), float(target_size), near_depth, far_depth};
    const D3D12_RECT scissor{0, 0, LONG(target_size), LONG(target_size)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    const float constants[2] = {c.z, c.depth};
    list->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    list->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER to_copy =
        transition(target.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = target.p;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.p;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    STEP(list->Close());
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    if (!wait(device, queue.p)) {
        std::fprintf(stderr, "the queue did not finish\n");
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE range{0, SIZE_T(total)};
    STEP(readback->Map(0, &range, &mapped));
    out.resize(target_size * target_size);
    for (UINT row = 0; row < target_size; ++row)
        std::memcpy(&out[row * target_size], static_cast<const std::uint8_t*>(mapped) + footprint.Offset +
                                              row * footprint.Footprint.RowPitch, target_size * sizeof(float));
    const D3D12_RANGE none{};
    readback->Unmap(0, &none);
    return true;
#undef STEP
}

const char* hlsl =
    "cbuffer Constants : register(b0) { float z; float depth; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "    return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, z, 1.0);\n"
    "}\n"
    "float ps() : SV_Depth { return depth; }\n";

bool compile(HMODULE compiler, const char* entry, const char* target, Ptr<ID3DBlob>& blob) {
    using Compile = decltype(&D3DCompile);
    const auto fn = reinterpret_cast<Compile>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")));
    if (!fn) return false;
    Ptr<ID3DBlob> errors;
    const HRESULT hr = fn(hlsl, std::strlen(hlsl), "depth_clamp.hlsl", nullptr, nullptr, entry, target, 0, 0, &blob.p,
                          &errors.p);
    if (FAILED(hr) && errors.p) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    return SUCCEEDED(hr) && blob.p;
}

// Removes the folder the layer's shaders were copied to, whichever way main returns.
struct TempFolder {
    std::filesystem::path path;
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

const char* side(bool clip) { return clip ? "TRUE" : "FALSE"; }

}  // namespace

int main() {
    vktest::Device vk;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) {
        std::printf("SKIP d3d12_depth_clamp: no Vulkan device\n");
        return 77;
    }
    if (status != vktest::DeviceStatus::ready) return 1;
    if (!vk.depth_clip_enable || !vk.enabled.depthClamp) {
        std::printf("SKIP d3d12_depth_clamp: the Vulkan device lacks %s\n",
                    !vk.enabled.depthClamp ? "depthClamp" : "VK_EXT_depth_clip_enable");
        return 77;
    }
    // Under Wine the host is vkd3d, which is not the reference: only the layer is checked, against the table. On
    // Windows a missing reference is a failure (it always has WARP, and d3dcompiler_47.dll).
    const bool need_host = !hostwine::host_is_wine();
    Ptr<ID3D12Device> host;
    std::string host_name;
    Ptr<ID3DBlob> host_vs, host_ps;
    if (need_host) {
        if (!copycmp::create_host(host, host_name)) {
            std::fprintf(stderr, "no D3D12 reference device (default adapter or WARP)\n");
            return 1;
        }
        const HMODULE compiler = LoadLibraryExA("d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!compiler) {
            std::fprintf(stderr, "d3dcompiler_47.dll does not load\n");
            return 1;
        }
        if (!compile(compiler, "vs", "vs_5_0", host_vs) || !compile(compiler, "ps", "ps_5_0", host_ps)) {
            std::fprintf(stderr, "the HLSL does not compile\n");
            return 1;
        }
        std::printf("host: %s\n", host_name.c_str());
    }
    const unsigned errors_before = vk.validation_errors();

    // The layer's shaders: programs and containers shaped like the game's, and the index that maps their hashes to
    // the compiled GLSL.
    const auto vs_program = d3d12test::fssm_program({0x56530011, 0xbf810000});
    const auto ps_program = d3d12test::fssm_program({0x50530011, 0xbf810000});
    const auto vs_container = d3d12test::shader_container(vs_program);
    const auto ps_container = d3d12test::shader_container(ps_program);
    const TempFolder library{std::filesystem::temp_directory_path() / "leonida_depth_clamp_test"};
    std::filesystem::create_directories(library.path);
    std::filesystem::copy_file(LEONIDA_TEST_VS_SPV, library.path / "vs_depth_clamp.spv",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_PS_SPV, library.path / "ps_depth_clamp.spv",
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream index(library.path / "index.json", std::ios::binary);
        index << "{\"vs_depth_clamp\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": []},\n"
                 " \"ps_depth_clamp\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": []}}";
    }

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants = {0, 0, 2};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    root_desc.Desc_1_0 = {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const auto root_blob = d3d12test::serialize_root_signature(root_desc);
    if (root_blob.empty()) {
        std::fprintf(stderr, "the host cannot serialize the root signature\n");
        return 1;
    }

    unsigned failures = 0, compared = 0, fallback_checked = 0;
    const Shaders layer_shaders{{vs_container.data(), vs_container.size()}, {ps_container.data(), ps_container.size()}};
    const Shaders host_shaders = need_host ? Shaders{{host_vs->GetBufferPointer(), host_vs->GetBufferSize()},
                                                     {host_ps->GetBufferPointer(), host_ps->GetBufferSize()}}
                                           : Shaders{};
    const auto make_layer = [&](bool extension, Ptr<ID3D12Device>& ours) {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = vk.instance_proc;
        info.instance = vk.instance;
        info.physical = vk.physical;
        info.device = vk.device;
        info.queue = vk.queue;
        info.queue_family = vk.family;
        info.buffer_device_address = true;
        info.shader_dir = library.path.string();
        info.features.depthClamp = vk.enabled.depthClamp;
        info.depth_clip_enable = extension;  // false: the layer treats the extension as absent
        if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&ours.p)))) {
            std::fprintf(stderr, "create_vulkan_device failed\n");
            return false;
        }
        return true;
    };
    const auto center = [](const std::vector<float>& v) {
        return v.empty() ? -1.0f : v[target_size * target_size / 2 + target_size / 2];
    };
    const auto all_equal = [](const std::vector<float>& v, float want) {
        return !v.empty() && std::all_of(v.begin(), v.end(), [&](float x) { return x == want; });
    };
    {
        Ptr<ID3D12Device> ours;
        if (!make_layer(true, ours)) return 1;
        for (const Case& c : cases) {
            for (const bool clip : {true, false}) {
                const float want = clip ? c.want_clipped : c.want_unclipped;
                std::vector<float> layer_pixels, host_pixels;
                if (!draw_and_read(ours.p, layer_shaders, root_blob, c, clip, layer_pixels) ||
                    (need_host && !draw_and_read(host.p, host_shaders, root_blob, c, clip, host_pixels))) {
                    std::fprintf(stderr, "%s, DepthClipEnable %s: a step failed\n", c.name, side(clip));
                    ++failures;
                    continue;
                }
                const bool alike = !need_host || (layer_pixels.size() == host_pixels.size() &&
                                   !std::memcmp(layer_pixels.data(), host_pixels.data(), layer_pixels.size() * 4));
                const bool host_as_measured = !need_host || all_equal(host_pixels, want);
                const bool layer_as_measured = all_equal(layer_pixels, want);
                std::printf("%-33s DepthClipEnable %-5s: layer %.9g, host %.9g, expected %.9g: %s\n", c.name,
                            side(clip), center(layer_pixels), need_host ? center(host_pixels) : -1.0f, want,
                            alike && host_as_measured && layer_as_measured ? "ok" : "MISMATCH");
                ++compared;
                if (!host_as_measured) std::fprintf(stderr, "  the host does not return the measured value\n");
                if (!alike || !layer_as_measured) ++failures;
            }
        }

        // An inverted viewport (MinDepth 0.5, MaxDepth 0.25). Measured: the default adapter (NVIDIA) clamps to the
        // range between the two (0.5, 0.25 and 0.4 for SV_Depth 0.9, 0.1 and 0.4), and WARP writes 0.25 for all of
        // them, so the two hosts disagree and there is no reference to hold the layer to. Printed for information,
        // not part of pass or fail.
        for (const Case& c : {cases[0], cases[1], cases[2]}) {
            for (const bool clip : {true, false}) {
                std::vector<float> layer_pixels, host_pixels;
                const bool ran =
                    draw_and_read(ours.p, layer_shaders, root_blob, c, clip, layer_pixels, max_depth, min_depth) &&
                    (!need_host ||
                     draw_and_read(host.p, host_shaders, root_blob, c, clip, host_pixels, max_depth, min_depth));
                if (ran)
                    std::printf("inverted viewport, %-27s DepthClipEnable %-5s: layer %.9g, host %.9g "
                                "(information only)\n", c.name, side(clip), center(layer_pixels),
                                need_host ? center(host_pixels) : -1.0f);
            }
        }
    }

    // Without VK_EXT_depth_clip_enable (the layer is told it is absent): FALSE still clamps through depthClamp, and
    // the cases that clip or stay inside the range are unchanged. TRUE clips but leaves an out-of-range SV_Depth or
    // biased depth unclamped, which is the documented behaviour of that mode.
    {
        Ptr<ID3D12Device> ours;
        if (!make_layer(false, ours)) return 1;
        for (const Case& c : cases) {
            for (const bool clip : {true, false}) {
                std::vector<float> pixels;
                if (!draw_and_read(ours.p, layer_shaders, root_blob, c, clip, pixels)) {
                    std::fprintf(stderr, "%s, without the extension, DepthClipEnable %s: a step failed\n", c.name,
                                 side(clip));
                    ++failures;
                    continue;
                }
                bool ok;
                if (clip && c.fallback_side) {
                    const float v = center(pixels);
                    ok = (c.fallback_side > 0 ? v > max_depth : v < min_depth) && all_equal(pixels, v);
                } else {
                    ok = all_equal(pixels, clip ? c.want_clipped : c.want_unclipped);
                }
                std::printf("without the extension: %-33s DepthClipEnable %-5s: layer %.9g: %s\n", c.name,
                            side(clip), center(pixels), ok ? "ok" : "MISMATCH");
                ++fallback_checked;
                if (!ok) ++failures;
            }
        }
    }
    const unsigned validation_errors = vk.validation_errors() - errors_before;
    if (validation_errors) std::fprintf(stderr, "%u Vulkan validation error(s)\n", validation_errors);
    if (failures || validation_errors) return 1;
    if (need_host)
        std::printf("depth clamp: %u cases on the layer and on the host (%s) returned the same depth; %u without the "
                    "extension behaved as documented\n", compared, host_name.c_str(), fallback_checked);
    else
        std::printf("depth clamp: %u cases on the layer matched the measured values (no host comparison under "
                    "Wine); %u without the extension behaved as documented\n", compared, fallback_checked);
    return 0;
}
