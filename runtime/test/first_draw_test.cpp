// First draw: a real Vulkan triangle through the bindless set and a graphics pipeline (milestone M1).
//
// The test is deliberately independent of the game: two hand-written SPIR-V shaders (first_draw_shaders.h) draw a
// triangle over an orange clear into an offscreen RGBA8 image, the image is copied to a host-visible buffer and the
// pixels are checked. The pipeline layout uses the bindless set 0 layout and the command buffer binds the bindless
// descriptor set, so a layout/set mismatch fails here.
//
// No game data, no keys. Exit 77 means the machine has no usable Vulkan device (the other GPU tests skip the same
// way).

#include "vk_test_device.h"

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <cstring>
#include <vector>

#include "first_draw_shaders.h"
#include "vulkan_bindless.h"

namespace {

constexpr std::uint32_t kSize = 64;

VkShaderModule make_shader(vktest::Device& device, const std::uint32_t* words, std::size_t count) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = count * sizeof(std::uint32_t);
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    if (device.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device.device, &info, nullptr, &module) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

int fail(const char* what) {
    std::fprintf(stderr, "first_draw_test: %s\n", what);
    return 1;
}

}  // namespace

int main() {
    vktest::Device device;
    if (!device.create()) {
        std::printf("first_draw_test: no Vulkan device\n");
        return 77;
    }

    VkPhysicalDeviceMemoryProperties memory{};
    device.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
        device.physical, &memory);

    // The bindless set is the pipeline layout's set 0 and the set bound before the draw.
    leonida::BindlessSet bindless(device.device_proc, device.device, memory);
    if (!bindless.ok()) return fail("BindlessSet creation failed");

    const VkImage image =
        device.image(VK_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!image) return fail("offscreen image creation failed");

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (device.fn<PFN_vkCreateImageView>("vkCreateImageView")(device.device, &view_info, nullptr, &view) != VK_SUCCESS)
        return fail("image view creation failed");

    const VkShaderModule vert = make_shader(device, first_draw::triangle_vert_spv,
                                            std::size(first_draw::triangle_vert_spv));
    const VkShaderModule frag = make_shader(device, first_draw::triangle_frag_spv,
                                            std::size(first_draw::triangle_frag_spv));
    if (!vert || !frag) return fail("shader module creation failed");

    const VkDescriptorSetLayout set_layout = bindless.layout();
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    if (device.fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device.device, &layout_info, nullptr,
                                                                        &pipeline_layout) != VK_SUCCESS)
        return fail("pipeline layout creation failed");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.stride = 2 * sizeof(float);
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vertex_attribute{};
    vertex_attribute.location = 0;
    vertex_attribute.binding = 0;
    vertex_attribute.format = VK_FORMAT_R32G32_SFLOAT;
    vertex_attribute.offset = 0;
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 1;
    vertex_input.pVertexAttributeDescriptions = &vertex_attribute;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &view_info.format;

    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.pNext = &rendering;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (device.fn<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(
            device.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS)
        return fail("graphics pipeline creation failed");

    void* mapped = nullptr;
    const VkBuffer readback = device.buffer(VkDeviceSize(kSize) * kSize * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mapped);
    if (!readback) return fail("readback buffer creation failed");

    // The triangle's three vertices come from a vertex buffer now, not from gl_VertexIndex.
    const float vertices[6] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};
    void* vertex_data = nullptr;
    const VkBuffer vertex_buffer = device.buffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertex_data);
    if (!vertex_buffer) return fail("vertex buffer creation failed");
    std::memcpy(vertex_data, vertices, sizeof(vertices));

    const bool recorded = device.run([&](VkCommandBuffer command_buffer) {
        bindless.record_fallback_init(command_buffer);
        device.initial_layout(command_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
        VkRenderingInfo rendering_info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering_info.renderArea = {{0, 0}, {kSize, kSize}};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color;

        device.fn<PFN_vkCmdBeginRendering>("vkCmdBeginRendering")(command_buffer, &rendering_info);
        device.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{0.0f, 0.0f, float(kSize), float(kSize), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {kSize, kSize}};
        device.fn<PFN_vkCmdSetViewport>("vkCmdSetViewport")(command_buffer, 0, 1, &viewport);
        device.fn<PFN_vkCmdSetScissor>("vkCmdSetScissor")(command_buffer, 0, 1, &scissor);
        const VkDeviceSize vertex_offset = 0;
        device.fn<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")(command_buffer, 0, 1, &vertex_buffer,
                                                                       &vertex_offset);
        const VkDescriptorSet set = bindless.set();
        device.fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
            command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 0, nullptr);
        device.fn<PFN_vkCmdDraw>("vkCmdDraw")(command_buffer, 3, 1, 0, 0);
        device.fn<PFN_vkCmdEndRendering>("vkCmdEndRendering")(command_buffer);

        VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_copy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_copy.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_copy.image = image;
        to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        device.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
            command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
            nullptr, 0, nullptr, 1, &to_copy);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kSize, kSize, 1};
        device.fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
            command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        return true;
    });
    if (!recorded) return fail("command buffer recording/submit failed");

    const auto* pixels = static_cast<const std::uint8_t*>(mapped);
    std::size_t non_black = 0, orange = 0;
    for (std::uint32_t i = 0; i < kSize * kSize; ++i) {
        const std::uint8_t r = pixels[i * 4], g = pixels[i * 4 + 1], b = pixels[i * 4 + 2];
        if (r || g || b) ++non_black;
        if (r > 200 && g > 30 && g < 120 && b < 40) ++orange;  // the shader's vec4(1.0, 0.25, 0.0, 1.0)
    }
    std::printf("first_draw_test: %zu/%u pixels lit, %zu orange\n", non_black, kSize * kSize, orange);

    device.fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device.device, pipeline, nullptr);
    device.fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device.device, pipeline_layout, nullptr);
    device.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device.device, vert, nullptr);
    device.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device.device, frag, nullptr);
    device.fn<PFN_vkDestroyImageView>("vkDestroyImageView")(device.device, view, nullptr);

    if (!non_black) return fail("the rendered image is black: the draw did not reach the attachment");
    if (!orange) return fail("no pixel has the shader's orange");
    std::printf("first_draw_test: all tests passed\n");
    return 0;
}
