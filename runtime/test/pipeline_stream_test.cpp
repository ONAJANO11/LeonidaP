// Unit tests for the D3D12 pipeline-state stream parser.
//
// A synthetic stream is built the way D3D12's helper builds one (8-byte type header, payload, rounded to 8) and the
// parser must recover the shaders, blend/depth/rasterizer pointers, input layout, topology, formats and sample count.
// Negative cases cover a truncated stream, a duplicate VS and an unknown subobject.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pipeline_stream.h"

namespace {

int failures = 0;

#define EXPECT(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

using rt::d3d12::parse_pipeline_stream;
using rt::d3d12::PipelineStream;

template <typename T>
void add(std::vector<std::uint8_t>& stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T& payload) {
    const std::size_t start = stream.size();
    const std::size_t total = (8 + sizeof(T) + 7) & ~std::size_t(7);
    stream.resize(start + total, 0);
    std::memcpy(stream.data() + start, &type, sizeof(type));
    std::memcpy(stream.data() + start + 8, &payload, sizeof(T));
}

int test_parses_a_full_stream() {
    static const std::uint32_t vs[] = {0x07230203, 1, 2};
    static const std::uint32_t ps[] = {0x07230203, 3, 4};
    D3D12_SHADER_BYTECODE vs_code{vs, sizeof(vs)};
    D3D12_SHADER_BYTECODE ps_code{ps, sizeof(ps)};
    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    D3D12_RASTERIZER_DESC raster{};
    raster.CullMode = D3D12_CULL_MODE_NONE;
    D3D12_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    D3D12_INPUT_LAYOUT_DESC layout{&element, 1};
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.NumRenderTargets = 1;
    formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    std::vector<std::uint8_t> stream;
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, vs_code);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, ps_code);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, layout);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, depth);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, raster);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT_UNKNOWN);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC{1, 0});

    PipelineStream parsed{};
    std::string error;
    EXPECT(parse_pipeline_stream(stream.data(), stream.size(), parsed, &error));
    EXPECT(error.empty());
    EXPECT(parsed.vs && parsed.vs->pShaderBytecode == vs && parsed.vs->BytecodeLength == sizeof(vs));
    EXPECT(parsed.ps && parsed.ps->BytecodeLength == sizeof(ps));
    EXPECT(parsed.input_layout && parsed.input_layout->NumElements == 1);
    EXPECT(parsed.input_layout->pInputElementDescs[0].InputSlot == 0);
    EXPECT(parsed.blend && parsed.blend->AlphaToCoverageEnable == TRUE);
    EXPECT(parsed.depth_stencil && parsed.depth_stencil->DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO);
    EXPECT(parsed.rasterizer && parsed.rasterizer->CullMode == D3D12_CULL_MODE_NONE);
    EXPECT(parsed.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    EXPECT(parsed.rtv_count == 1 && parsed.rtv_formats[0] == DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT(parsed.dsv_format == DXGI_FORMAT_UNKNOWN);
    EXPECT(parsed.sample.Count == 1);
    return failures;
}

int test_truncated_stream_is_rejected() {
    static const std::uint32_t vs[] = {0x07230203, 1};
    D3D12_SHADER_BYTECODE code{vs, sizeof(vs)};
    std::vector<std::uint8_t> stream;
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, code);
    add(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, code);
    PipelineStream parsed{};
    std::string error;
    EXPECT(!parse_pipeline_stream(stream.data(), stream.size() - 12, parsed, &error));
    EXPECT(!error.empty());
    return failures;
}

int test_duplicate_and_unknown_are_rejected() {
    static const std::uint32_t vs[] = {0x07230203, 1};
    D3D12_SHADER_BYTECODE code{vs, sizeof(vs)};
    std::vector<std::uint8_t> duplicate;
    add(duplicate, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, code);
    add(duplicate, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, code);
    PipelineStream parsed{};
    std::string error;
    EXPECT(!parse_pipeline_stream(duplicate.data(), duplicate.size(), parsed, &error));

    std::vector<std::uint8_t> unknown;
    add(unknown, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, code);
    add(unknown, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, code);
    const auto bad_type = static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(0x7ff);
    add(unknown, bad_type, std::uint32_t{0});
    EXPECT(!parse_pipeline_stream(unknown.data(), unknown.size(), parsed, &error));
    return failures;
}

int test_missing_shaders_are_rejected() {
    std::vector<std::uint8_t> empty_stream;
    PipelineStream parsed{};
    std::string error;
    EXPECT(!parse_pipeline_stream(nullptr, 0, parsed, &error));
    EXPECT(!parse_pipeline_stream(empty_stream.data(), 0, parsed, &error));
    return failures;
}

}  // namespace

int main() {
    test_parses_a_full_stream();
    test_truncated_stream_is_rejected();
    test_duplicate_and_unknown_are_rejected();
    test_missing_shaders_are_rejected();

    if (failures) {
        std::fprintf(stderr, "pipeline_stream_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pipeline_stream_test: all tests passed\n");
    return 0;
}
