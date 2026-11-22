// VK_KHR_portability_subset: what a device that only offers a subset of Vulkan (MoltenVK) leaves out. Only the parts
// the runtime and the test tools need. The extension is a provisional one: <vulkan/vulkan_beta.h> declares its
// structure and sType only under VK_ENABLE_BETA_EXTENSIONS, so they are spelt out here.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace leonida {

inline constexpr const char* kPortabilitySubsetExtension = "VK_KHR_portability_subset";
inline constexpr auto kPortabilitySubsetFeaturesType = VkStructureType(1000163000);

// VkPhysicalDevicePortabilitySubsetFeaturesKHR, member for member.
struct PortabilitySubsetFeatures {
    VkStructureType sType = kPortabilitySubsetFeaturesType;
    void* pNext = nullptr;
    VkBool32 constantAlphaColorBlendFactors = VK_FALSE;
    VkBool32 events = VK_FALSE;
    VkBool32 imageViewFormatReinterpretation = VK_FALSE;
    VkBool32 imageViewFormatSwizzle = VK_FALSE;
    VkBool32 imageView2DOn3DImage = VK_FALSE;
    VkBool32 multisampleArrayImage = VK_FALSE;
    VkBool32 mutableComparisonSamplers = VK_FALSE;
    VkBool32 pointPolygons = VK_FALSE;
    VkBool32 samplerMipLodBias = VK_FALSE;
    VkBool32 separateStencilMaskRef = VK_FALSE;
    VkBool32 shaderSampleRateInterpolationFunctions = VK_FALSE;
    VkBool32 tessellationIsolines = VK_FALSE;
    VkBool32 tessellationPointMode = VK_FALSE;
    VkBool32 triangleFans = VK_FALSE;
    VkBool32 vertexAttributeAccessBeyondStride = VK_FALSE;
};

// Whether `physical` offers VK_KHR_portability_subset. A device that does must have it enabled
// (VUID-VkDeviceCreateInfo-pProperties-04451), and only such a device may lack a feature it lists.
inline bool has_portability_subset(PFN_vkEnumerateDeviceExtensionProperties enumerate, VkPhysicalDevice physical) {
    if (!enumerate) return false;
    std::uint32_t count = 0;
    if (enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS || !count) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (enumerate(physical, nullptr, &count, extensions.data()) < VK_SUCCESS) return false;
    for (std::uint32_t i = 0; i < count && i < extensions.size(); ++i)
        if (!std::strcmp(extensions[i].extensionName, kPortabilitySubsetExtension)) return true;
    return false;
}

// Whether `physical` is MoltenVK, by its driver ID. Under Wine, winevulkan doesn't pass VK_KHR_portability_subset on
// to Windows programs (it isn't a Windows extension), but it does pass the host driver's ID: a program there can only
// tell MoltenVK, and so the subset's missing features, this way.
inline bool is_moltenvk(PFN_vkGetPhysicalDeviceProperties2 properties2, VkPhysicalDevice physical) {
    if (!properties2) return false;
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    properties2(physical, &properties);
    return driver.driverID == VK_DRIVER_ID_MOLTENVK;
}

// Whether `physical`'s samplers take a LOD bias (VkSamplerCreateInfo::mipLodBias). Every conformant device's do. A
// portability-subset device's do when it reports samplerMipLodBias; MoltenVK does not (Metal samplers have no bias),
// and there a non-zero mipLodBias is invalid (VUID-VkSamplerCreateInfo-samplerMipLodBias-04467) and ignored. The
// translated shaders then apply the S#'s bias themselves (tools/rdna2spirv.py --lod-bias-fallback).
// With `properties2`, MoltenVK without the extension in sight (under Wine, is_moltenvk) counts as one without the bias.
inline bool sampler_mip_lod_bias(PFN_vkEnumerateDeviceExtensionProperties enumerate,
                                 PFN_vkGetPhysicalDeviceFeatures2 features2, VkPhysicalDevice physical,
                                 PFN_vkGetPhysicalDeviceProperties2 properties2 = nullptr) {
    if (!has_portability_subset(enumerate, physical)) return !is_moltenvk(properties2, physical);
    if (!features2) return false;
    PortabilitySubsetFeatures subset{};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &subset};
    features2(physical, &features);
    return subset.samplerMipLodBias == VK_TRUE;
}

}  // namespace leonida
