#pragma once

// D3D12 pipeline-state stream parser (the input to CreatePipelineState).
//
// A stream is a sequence of 8-byte-aligned subobjects: a D3D12_PIPELINE_STATE_SUBOBJECT_TYPE, four bytes of padding,
// then the subobject's struct. This parser walks the stream, takes the pointers to the subobjects it understands and
// records the small inline values (topology, render-target formats, sample count). It does not copy the payloads: the
// returned struct points into the stream, which must outlive it.
//
// It is deliberately independent of Vulkan and of the device, so a synthetic stream can be parsed in a CPU test.
// Unknown or duplicated subobjects are reported as errors instead of being skipped silently: a stream that names two
// vertex shaders is a caller bug, and a wrong size is a corrupt stream.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <d3d12.h>

namespace rt::d3d12 {

struct PipelineStream {
    const D3D12_SHADER_BYTECODE* vs = nullptr;
    const D3D12_SHADER_BYTECODE* ps = nullptr;
    const D3D12_SHADER_BYTECODE* cs = nullptr;
    const D3D12_INPUT_LAYOUT_DESC* input_layout = nullptr;
    const D3D12_BLEND_DESC* blend = nullptr;
    const D3D12_DEPTH_STENCIL_DESC* depth_stencil = nullptr;
    const D3D12_RASTERIZER_DESC* rasterizer = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    DXGI_SAMPLE_DESC sample{1, 0};
    DXGI_FORMAT rtv_formats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    std::uint32_t rtv_count = 0;
    DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
    bool has_depth_stencil = false;
};

namespace detail {

constexpr std::size_t align8(std::size_t size) { return (size + 7) & ~std::size_t(7); }

// Total bytes a subobject with `payload` occupies: 8-byte type header + payload, rounded to 8.
template <typename T>
constexpr std::size_t subobject_size() {
    return align8(8 + sizeof(T));
}

inline std::string type_name(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: return "VS";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: return "PS";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: return "CS";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return "InputLayout";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return "Blend";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return "DepthStencil";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return "Rasterizer";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return "PrimitiveTopology";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return "RenderTargetFormats";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return "DepthStencilFormat";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return "SampleDesc";
        default: return "type " + std::to_string(static_cast<unsigned>(type));
    }
}

}  // namespace detail

// Parses `data` (`size` bytes). Returns false and sets `error` on a truncated stream, an unknown subobject type, a
// duplicated subobject, or a missing vertex/pixel shader. The stream is the D3D12_PIPELINE_STATE_STREAM_DESC's
// pPipelineStateSubobjectStream/SizeInBytes.
inline bool parse_pipeline_stream(const void* data, std::size_t size, PipelineStream& out,
                                  std::string* error = nullptr) {
    out = PipelineStream{};
    if (!data || size < 8) {
        if (error) *error = "empty pipeline stream";
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    bool saw_vs = false, saw_ps = false;
    while (offset + 8 <= size) {
        const auto type = static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(
            *reinterpret_cast<const std::uint32_t*>(bytes + offset));
        const std::uint8_t* payload = bytes + offset + 8;
        const std::size_t remaining = size - offset - 8;
        std::size_t consumed = 0;

        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
                if (saw_vs) { if (error) *error = "duplicate VS subobject"; return false; }
                if (remaining < sizeof(D3D12_SHADER_BYTECODE)) { if (error) *error = "truncated VS"; return false; }
                out.vs = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(payload);
                saw_vs = true;
                consumed = detail::subobject_size<D3D12_SHADER_BYTECODE>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
                if (saw_ps) { if (error) *error = "duplicate PS subobject"; return false; }
                if (remaining < sizeof(D3D12_SHADER_BYTECODE)) { if (error) *error = "truncated PS"; return false; }
                out.ps = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(payload);
                saw_ps = true;
                consumed = detail::subobject_size<D3D12_SHADER_BYTECODE>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
                if (remaining < sizeof(D3D12_SHADER_BYTECODE)) { if (error) *error = "truncated CS"; return false; }
                out.cs = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(payload);
                consumed = detail::subobject_size<D3D12_SHADER_BYTECODE>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
                if (remaining < sizeof(D3D12_INPUT_LAYOUT_DESC)) { if (error) *error = "truncated InputLayout"; return false; }
                out.input_layout = reinterpret_cast<const D3D12_INPUT_LAYOUT_DESC*>(payload);
                consumed = detail::subobject_size<D3D12_INPUT_LAYOUT_DESC>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
                if (remaining < sizeof(D3D12_BLEND_DESC)) { if (error) *error = "truncated Blend"; return false; }
                out.blend = reinterpret_cast<const D3D12_BLEND_DESC*>(payload);
                consumed = detail::subobject_size<D3D12_BLEND_DESC>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
                if (remaining < sizeof(D3D12_DEPTH_STENCIL_DESC)) { if (error) *error = "truncated DepthStencil"; return false; }
                out.depth_stencil = reinterpret_cast<const D3D12_DEPTH_STENCIL_DESC*>(payload);
                out.has_depth_stencil = true;
                consumed = detail::subobject_size<D3D12_DEPTH_STENCIL_DESC>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
                if (remaining < sizeof(D3D12_RASTERIZER_DESC)) { if (error) *error = "truncated Rasterizer"; return false; }
                out.rasterizer = reinterpret_cast<const D3D12_RASTERIZER_DESC*>(payload);
                consumed = detail::subobject_size<D3D12_RASTERIZER_DESC>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
                if (remaining < sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE)) { if (error) *error = "truncated PrimitiveTopology"; return false; }
                out.topology = *reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY_TYPE*>(payload);
                consumed = detail::subobject_size<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
                if (remaining < sizeof(D3D12_RT_FORMAT_ARRAY)) { if (error) *error = "truncated RenderTargetFormats"; return false; }
                const auto* formats = reinterpret_cast<const D3D12_RT_FORMAT_ARRAY*>(payload);
                const std::uint32_t count = std::min<std::uint32_t>(formats->NumRenderTargets,
                                                                    D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
                out.rtv_count = count;
                for (std::uint32_t i = 0; i < count; ++i) out.rtv_formats[i] = formats->RTFormats[i];
                consumed = detail::subobject_size<D3D12_RT_FORMAT_ARRAY>();
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
                if (remaining < sizeof(DXGI_FORMAT)) { if (error) *error = "truncated DepthStencilFormat"; return false; }
                out.dsv_format = *reinterpret_cast<const DXGI_FORMAT*>(payload);
                consumed = detail::subobject_size<DXGI_FORMAT>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
                if (remaining < sizeof(DXGI_SAMPLE_DESC)) { if (error) *error = "truncated SampleDesc"; return false; }
                out.sample = *reinterpret_cast<const DXGI_SAMPLE_DESC*>(payload);
                consumed = detail::subobject_size<DXGI_SAMPLE_DESC>();
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
                consumed = detail::subobject_size<UINT>();
                break;
            default:
                if (error) *error = "unsupported subobject " + detail::type_name(type);
                return false;
        }
        if (consumed == 0 || offset + consumed > size) {
            if (error) *error = "subobject overruns the stream: " + detail::type_name(type);
            return false;
        }
        offset += consumed;
    }
    if (!saw_vs) {
        if (error) *error = "no vertex shader subobject";
        return false;
    }
    if (!saw_ps && !out.cs) {
        if (error) *error = "no pixel or compute shader subobject";
        return false;
    }
    return true;
}

}  // namespace rt::d3d12
