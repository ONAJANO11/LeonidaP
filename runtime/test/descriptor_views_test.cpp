// Unit tests for the D3D12 view -> GFX10 T#/V# encoders (P2 of docs/d3d12_vulkan_analysis.md).
//
// No GPU and no game data: each case builds a D3D12 view description, encodes it, decodes the words back with
// gfx10::decode and compares the fields the shader will read. The Vulkan headers are needed for VkFormat; the
// test never creates a Vulkan object.

#include <cstdio>
#include <cstdint>

#include "descriptor_views.h"
#include "gfx10_descriptors.h"

namespace {

int failures = 0;

#define EXPECT(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

using leonida::gfx10::Buffer;
using leonida::gfx10::BufferWords;
using leonida::gfx10::Image;
using leonida::gfx10::ImageWords;

constexpr UINT kIdentityMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

int test_constant_buffer() {
    BufferWords words{};
    EXPECT(rt::d3d12::views::encode_cbv(0x12345000, 0x1000, words) == true);
    Buffer decoded{};
    EXPECT(leonida::gfx10::decode(words, decoded));
    EXPECT(decoded.address == 0x12345000);
    EXPECT(decoded.stride == 0);
    EXPECT(decoded.num_records == 0x1000);
    EXPECT(decoded.format == 0);
    // A CBV with no address or no size is rejected, not encoded as slot 0 of the VA space.
    EXPECT(rt::d3d12::views::encode_cbv(0, 0x1000, words) == false);
    EXPECT(rt::d3d12::views::encode_cbv(0x1000, 0, words) == false);
    return failures;
}

int test_structured_buffer_srv() {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = kIdentityMapping;
    desc.Buffer.FirstElement = 2;
    desc.Buffer.NumElements = 10;
    desc.Buffer.StructureByteStride = 16;

    const rt::d3d12::views::BufferResource resource{0x20000000, 4096};
    BufferWords words{};
    EXPECT(rt::d3d12::views::encode_buffer_srv(desc, resource, VK_FORMAT_UNDEFINED, words));
    Buffer decoded{};
    EXPECT(leonida::gfx10::decode(words, decoded));
    EXPECT(decoded.address == 0x20000000 + 2 * 16);
    EXPECT(decoded.stride == 16);
    EXPECT(decoded.num_records == 10);
    EXPECT(decoded.format == 0);
    return failures;
}

int test_raw_buffer_srv_counts_bytes() {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = kIdentityMapping;
    desc.Buffer.FirstElement = 8;  // a byte offset for a raw buffer
    desc.Buffer.NumElements = 4;
    desc.Buffer.StructureByteStride = 0;

    const rt::d3d12::views::BufferResource resource{0x30000000, 1024};
    BufferWords words{};
    EXPECT(rt::d3d12::views::encode_buffer_srv(desc, resource, VK_FORMAT_UNDEFINED, words));
    Buffer decoded{};
    EXPECT(leonida::gfx10::decode(words, decoded));
    EXPECT(decoded.address == 0x30000000 + 8);
    EXPECT(decoded.stride == 0);
    EXPECT(decoded.num_records == 1024);  // bytes, not elements
    EXPECT(decoded.oob_select == leonida::gfx10::OobSelect::raw);
    return failures;
}

int test_typed_buffer_srv_sets_format() {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = kIdentityMapping;
    desc.Buffer.NumElements = 64;
    desc.Buffer.StructureByteStride = 0;

    const rt::d3d12::views::BufferResource resource{0x40000000, 1024};
    BufferWords words{};
    EXPECT(rt::d3d12::views::encode_buffer_srv(desc, resource, VK_FORMAT_R32G32B32A32_SFLOAT, words));
    Buffer decoded{};
    EXPECT(leonida::gfx10::decode(words, decoded));
    EXPECT(decoded.format != 0);
    EXPECT(decoded.num_records == 64);
    return failures;
}

int test_buffer_uav() {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = 1;
    desc.Buffer.NumElements = 32;
    desc.Buffer.StructureByteStride = 8;

    const rt::d3d12::views::BufferResource resource{0x50000000, 2048};
    BufferWords words{};
    EXPECT(rt::d3d12::views::encode_buffer_uav(desc, resource, VK_FORMAT_UNDEFINED, words));
    Buffer decoded{};
    EXPECT(leonida::gfx10::decode(words, decoded));
    EXPECT(decoded.address == 0x50000000 + 8);
    EXPECT(decoded.stride == 8);
    EXPECT(decoded.num_records == 32);
    return failures;
}

int test_texture_2d_srv_range() {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = kIdentityMapping;
    desc.Texture2D.MostDetailedMip = 2;
    desc.Texture2D.MipLevels = 3;

    rt::d3d12::views::TextureResource resource{};
    resource.address = 0x60000000;
    resource.width = 1024;
    resource.height = 512;
    resource.mip_levels = 10;

    ImageWords words{};
    EXPECT(rt::d3d12::views::encode_texture_srv(desc, resource, VK_FORMAT_R8G8B8A8_UNORM, words));
    Image decoded{};
    EXPECT(leonida::gfx10::decode(words, decoded));
    EXPECT(decoded.address == 0x60000000);
    EXPECT(decoded.type == leonida::gfx10::ImageType::image_2d);
    EXPECT(decoded.width == 1024 && decoded.height == 512);
    EXPECT(decoded.first_mip == 2);
    EXPECT(decoded.mip_count == 3);
    EXPECT(decoded.resource_mips == 10);
    EXPECT(decoded.format != 0);
    return failures;
}

int test_texture_cube_and_3d() {
    D3D12_SHADER_RESOURCE_VIEW_DESC cube{};
    cube.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    cube.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    cube.Shader4ComponentMapping = kIdentityMapping;
    cube.TextureCube.MostDetailedMip = 0;
    cube.TextureCube.MipLevels = 1;

    rt::d3d12::views::TextureResource resource{};
    resource.address = 0x70000000;
    resource.width = 256;
    resource.height = 256;
    resource.mip_levels = 8;
    resource.array_layers = 6;

    ImageWords words{};
    EXPECT(rt::d3d12::views::encode_texture_srv(cube, resource, VK_FORMAT_R16G16B16A16_SFLOAT, words));
    Image decoded{};
    EXPECT(leonida::gfx10::decode(words, decoded));
    EXPECT(decoded.type == leonida::gfx10::ImageType::cube);
    EXPECT(decoded.layer_count == 6);

    D3D12_UNORDERED_ACCESS_VIEW_DESC volume{};
    volume.Format = DXGI_FORMAT_R32_FLOAT;
    volume.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    volume.Texture3D.MipSlice = 1;
    rt::d3d12::views::TextureResource volume_resource{};
    volume_resource.address = 0x71000000;
    volume_resource.width = 64;
    volume_resource.height = 64;
    volume_resource.depth = 32;
    volume_resource.mip_levels = 4;
    EXPECT(rt::d3d12::views::encode_texture_uav(volume, volume_resource, VK_FORMAT_R32_SFLOAT, words));
    EXPECT(leonida::gfx10::decode(words, decoded));
    EXPECT(decoded.type == leonida::gfx10::ImageType::image_3d);
    EXPECT(decoded.depth == 32);
    EXPECT(decoded.first_mip == 1);
    return failures;
}

int test_rejections() {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    BufferWords words{};
    rt::d3d12::views::BufferResource empty{};
    EXPECT(rt::d3d12::views::encode_buffer_srv(desc, empty, VK_FORMAT_UNDEFINED, words) == false);
    rt::d3d12::views::BufferResource buffer{0x1000, 256};
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    EXPECT(rt::d3d12::views::encode_buffer_srv(desc, buffer, VK_FORMAT_UNDEFINED, words) == false);  // no format code
    D3D12_SHADER_RESOURCE_VIEW_DESC texture = desc;
    texture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ImageWords image{};
    rt::d3d12::views::TextureResource texture_resource{};
    texture_resource.address = 0x2000;
    EXPECT(rt::d3d12::views::encode_texture_srv(texture, texture_resource, VK_FORMAT_UNDEFINED, image) == false);
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    EXPECT(rt::d3d12::views::encode_texture_srv(texture, texture_resource, VK_FORMAT_R8G8B8A8_UNORM, image) == false);
    return failures;
}

}  // namespace

int main() {
    test_constant_buffer();
    test_structured_buffer_srv();
    test_raw_buffer_srv_counts_bytes();
    test_typed_buffer_srv_sets_format();
    test_buffer_uav();
    test_texture_2d_srv_range();
    test_texture_cube_and_3d();
    test_rejections();

    if (failures) {
        std::fprintf(stderr, "descriptor_views_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("descriptor_views_test: all tests passed\n");
    return 0;
}
