#pragma once

// D3D12 view descriptions -> GFX10 T#/V# words (P2 slice of docs/d3d12_vulkan_analysis.md: SRV/UAV/CBV).
//
// These are pure functions: no device, no Vulkan object, no game data. The device uses them to write a heap slot,
// and the bindless registration of the resulting view belongs to the #157 work. Keeping the encoding here lets the
// CPU tests pin it without a GPU: build a description, encode it, decode it back with gfx10::decode and compare.
//
// Scope: constant buffers and buffer SRV/UAV (raw, structured, typed) plus texture SRV/UAV. Typed buffers assume
// 32-bit elements (the translator's buffer_load/store paths), which the tests state explicitly.
//
// Assumptions that a trace can replace, all in one place:
//   - A CBV is a V# with stride 0 and NUM_RECORDS = size in bytes (the shader's raw-buffer rule).
//   - A raw buffer SRV/UAV counts bytes in NUM_RECORDS; a structured one counts records and sets STRIDE.
//   - Textures use the view's mip/layer range; the cube count is six layers a cube, starting at First2DArrayFace.

#include <d3d12.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>

#include "gfx10_descriptors.h"

namespace rt::d3d12::views {

struct BufferResource {
    std::uint64_t address = 0;  // console VA of the first byte
    std::uint64_t size = 0;     // bytes
};

struct TextureResource {
    std::uint64_t address = 0;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth = 1;        // 3D only
    std::uint32_t mip_levels = 1;   // of the whole resource
    std::uint32_t array_layers = 1;
    std::uint32_t samples = 1;
    bool depth_stencil = false;
};

namespace detail {

inline bool valid_swizzle_value(UINT value) {
    return value <= 1 || (value >= 4 && value <= 7);
}

inline leonida::gfx10::Swizzle swizzle(UINT mapping) {
    leonida::gfx10::Swizzle out{};
    for (unsigned i = 0; i < 4; ++i) {
        const UINT value = (mapping >> (3 * i)) & 0x7;
        if (!valid_swizzle_value(value)) return leonida::gfx10::identity_swizzle;
        out[i] = static_cast<std::uint8_t>(value);
    }
    return out;
}

inline bool is_buffer(D3D12_SRV_DIMENSION dimension) { return dimension == D3D12_SRV_DIMENSION_BUFFER; }
inline bool is_buffer(D3D12_UAV_DIMENSION dimension) { return dimension == D3D12_UAV_DIMENSION_BUFFER; }

inline leonida::gfx10::ImageType image_type(D3D12_SRV_DIMENSION dimension, std::uint32_t samples) {
    switch (dimension) {
        case D3D12_SRV_DIMENSION_TEXTURE1D: return leonida::gfx10::ImageType::image_1d;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: return leonida::gfx10::ImageType::image_1d_array;
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            return samples > 1 ? leonida::gfx10::ImageType::image_2d_msaa : leonida::gfx10::ImageType::image_2d;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            return samples > 1 ? leonida::gfx10::ImageType::image_2d_msaa_array
                               : leonida::gfx10::ImageType::image_2d_array;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS: return leonida::gfx10::ImageType::image_2d_msaa;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: return leonida::gfx10::ImageType::image_2d_msaa_array;
        case D3D12_SRV_DIMENSION_TEXTURE3D: return leonida::gfx10::ImageType::image_3d;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: return leonida::gfx10::ImageType::cube;
        default: return leonida::gfx10::ImageType::image_2d;
    }
}

}  // namespace detail

// A constant buffer view: a V# with no stride over [address, address + size).
inline bool encode_cbv(std::uint64_t address, std::uint32_t size, leonida::gfx10::BufferWords& out) {
    if (!address || !size) return false;
    leonida::gfx10::Buffer buffer{};
    buffer.address = address;
    buffer.stride = 0;
    buffer.num_records = size;  // bytes, as a raw buffer
    buffer.format = 0;
    buffer.oob_select = leonida::gfx10::OobSelect::raw;
    out = leonida::gfx10::encode(buffer);
    return true;
}

inline bool encode_buffer_srv(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, const BufferResource& resource,
                              VkFormat format, leonida::gfx10::BufferWords& out) {
    if (!detail::is_buffer(desc.ViewDimension) || !resource.address || !resource.size) return false;
    const D3D12_BUFFER_SRV& view = desc.Buffer;
    leonida::gfx10::Buffer buffer{};
    buffer.address = resource.address + std::uint64_t(view.FirstElement) * (view.StructureByteStride ? view.StructureByteStride : 1);
    if (view.StructureByteStride) {
        buffer.stride = view.StructureByteStride;
        buffer.num_records = view.NumElements;
    } else {
        buffer.stride = 0;
        buffer.num_records = static_cast<std::uint32_t>(std::min<std::uint64_t>(resource.size, 0xFFFFFFFFu));
    }
    if (desc.Format != DXGI_FORMAT_UNKNOWN) {
        buffer.format = leonida::gfx10::format_from_vulkan(format);
        if (!buffer.format) return false;
        // Typed buffer: the translator's buffer_load/store path reads 32-bit elements.
        buffer.num_records = view.NumElements ? view.NumElements
                                              : static_cast<std::uint32_t>(std::min<std::uint64_t>(resource.size / 4,
                                                                                                   0xFFFFFFFFu));
    }
    buffer.swizzle = detail::swizzle(desc.Shader4ComponentMapping);
    buffer.oob_select = buffer.stride ? leonida::gfx10::OobSelect::structured_with_offset
                                     : leonida::gfx10::OobSelect::raw;
    out = leonida::gfx10::encode(buffer);
    return true;
}

inline bool encode_buffer_uav(const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, const BufferResource& resource,
                              VkFormat format, leonida::gfx10::BufferWords& out) {
    if (!detail::is_buffer(desc.ViewDimension) || !resource.address || !resource.size) return false;
    const D3D12_BUFFER_UAV& view = desc.Buffer;
    leonida::gfx10::Buffer buffer{};
    buffer.address = resource.address + std::uint64_t(view.FirstElement) * (view.StructureByteStride ? view.StructureByteStride : 1);
    if (view.StructureByteStride) {
        buffer.stride = view.StructureByteStride;
        buffer.num_records = view.NumElements;
    } else {
        buffer.stride = 0;
        buffer.num_records = static_cast<std::uint32_t>(std::min<std::uint64_t>(resource.size, 0xFFFFFFFFu));
    }
    if (desc.Format != DXGI_FORMAT_UNKNOWN) {
        buffer.format = leonida::gfx10::format_from_vulkan(format);
        if (!buffer.format) return false;
        buffer.num_records = view.NumElements ? view.NumElements
                                              : static_cast<std::uint32_t>(std::min<std::uint64_t>(resource.size / 4,
                                                                                                   0xFFFFFFFFu));
    }
    buffer.oob_select = buffer.stride ? leonida::gfx10::OobSelect::structured_with_offset
                                     : leonida::gfx10::OobSelect::raw;
    out = leonida::gfx10::encode(buffer);
    return true;
}

inline bool encode_texture_srv(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, const TextureResource& resource,
                               VkFormat format, leonida::gfx10::ImageWords& out) {
    if (desc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER || !resource.address) return false;
    const std::uint32_t gfx10_format = leonida::gfx10::format_from_vulkan(format);
    if (!gfx10_format) return false;

    leonida::gfx10::Image image{};
    image.address = resource.address;
    image.format = gfx10_format;
    image.type = detail::image_type(desc.ViewDimension, resource.samples);
    image.width = std::max(resource.width, 1u);
    image.height = std::max(resource.height, 1u);
    image.depth = std::max(resource.depth, 1u);
    image.resource_mips = std::max(resource.mip_levels, 1u);
    image.samples = std::max(resource.samples, 1u);
    image.swizzle = detail::swizzle(desc.Shader4ComponentMapping);

    switch (desc.ViewDimension) {
        case D3D12_SRV_DIMENSION_TEXTURE1D:
            image.first_mip = desc.Texture1D.MostDetailedMip;
            image.mip_count = desc.Texture1D.MipLevels;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            image.first_mip = desc.Texture1DArray.MostDetailedMip;
            image.mip_count = desc.Texture1DArray.MipLevels;
            image.first_layer = desc.Texture1DArray.FirstArraySlice;
            image.layer_count = desc.Texture1DArray.ArraySize;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            image.first_mip = desc.Texture2D.MostDetailedMip;
            image.mip_count = desc.Texture2D.MipLevels;
            image.first_layer = desc.Texture2D.PlaneSlice;
            image.layer_count = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            image.first_mip = desc.Texture2DArray.MostDetailedMip;
            image.mip_count = desc.Texture2DArray.MipLevels;
            image.first_layer = desc.Texture2DArray.FirstArraySlice;
            image.layer_count = desc.Texture2DArray.ArraySize;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS:
            image.first_mip = 0;
            image.mip_count = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            image.first_mip = 0;
            image.mip_count = 1;
            image.first_layer = desc.Texture2DMSArray.FirstArraySlice;
            image.layer_count = desc.Texture2DMSArray.ArraySize;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            image.first_mip = desc.Texture3D.MostDetailedMip;
            image.mip_count = desc.Texture3D.MipLevels;
            image.first_layer = 0;
            image.layer_count = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            image.first_mip = desc.TextureCube.MostDetailedMip;
            image.mip_count = desc.TextureCube.MipLevels;
            image.first_layer = 0;
            image.layer_count = 6;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            image.first_mip = desc.TextureCubeArray.MostDetailedMip;
            image.mip_count = desc.TextureCubeArray.MipLevels;
            image.first_layer = desc.TextureCubeArray.First2DArrayFace;
            image.layer_count = 6 * desc.TextureCubeArray.NumCubes;
            break;
        default:
            return false;
    }
    if (!image.mip_count) image.mip_count = 1;
    out = leonida::gfx10::encode(image);
    return true;
}

inline bool encode_texture_uav(const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, const TextureResource& resource,
                               VkFormat format, leonida::gfx10::ImageWords& out) {
    if (desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER || !resource.address) return false;
    const std::uint32_t gfx10_format = leonida::gfx10::format_from_vulkan(format);
    if (!gfx10_format) return false;

    leonida::gfx10::Image image{};
    image.address = resource.address;
    image.format = gfx10_format;
    image.width = std::max(resource.width, 1u);
    image.height = std::max(resource.height, 1u);
    image.depth = std::max(resource.depth, 1u);
    image.resource_mips = std::max(resource.mip_levels, 1u);
    image.samples = std::max(resource.samples, 1u);

    switch (desc.ViewDimension) {
        case D3D12_UAV_DIMENSION_TEXTURE1D:
            image.type = leonida::gfx10::ImageType::image_1d;
            image.first_mip = desc.Texture1D.MipSlice;
            image.mip_count = 1;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
            image.type = leonida::gfx10::ImageType::image_1d_array;
            image.first_mip = desc.Texture1DArray.MipSlice;
            image.mip_count = 1;
            image.first_layer = desc.Texture1DArray.FirstArraySlice;
            image.layer_count = desc.Texture1DArray.ArraySize;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2D:
            image.type = leonida::gfx10::ImageType::image_2d;
            image.first_mip = desc.Texture2D.MipSlice;
            image.mip_count = 1;
            image.first_layer = desc.Texture2D.PlaneSlice;
            image.layer_count = 1;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            image.type = leonida::gfx10::ImageType::image_2d_array;
            image.first_mip = desc.Texture2DArray.MipSlice;
            image.mip_count = 1;
            image.first_layer = desc.Texture2DArray.FirstArraySlice;
            image.layer_count = desc.Texture2DArray.ArraySize;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE3D:
            image.type = leonida::gfx10::ImageType::image_3d;
            image.first_mip = desc.Texture3D.MipSlice;
            image.mip_count = 1;
            break;
        default:
            return false;
    }
    out = leonida::gfx10::encode(image);
    return true;
}

}  // namespace rt::d3d12::views
