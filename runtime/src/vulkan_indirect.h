// ExecuteIndirect with a multi-argument command signature, on VK_EXT_device_generated_commands (docs/d3d12_vulkan.md
// section 4.7). A D3D12 record is a run of arguments (root constants, root descriptors, vertex and index buffer views,
// then the draw or dispatch); Vulkan has no indirect call that changes that state per record, but a device-generated
// commands layout does: one token per argument, at the argument's byte offset in the record, and the record stride as
// the sequence stride. So the records the application wrote are the sequences, with two adjustments:
//  - root constants and root descriptors become push-constant tokens into the 144-byte block the translated shaders read
//    (user-data dwords s0..s31, vulkan_replay.h ShaderUserData): the dword offset of the root parameter in the root
//    signature gives the push-constant offset, so nothing is translated;
//  - a vertex or index buffer view holds a console virtual address, and a token wants a device address. A signature
//    with such an argument runs a compute pass first (indirect_patch.comp) that copies the records to scratch and adds
//    the arena's device address (heap_base) to each view; its sequences are the copy. Other signatures read the
//    application's buffer in place.
// The count buffer is the sequence count address (the count is clamped to MaxCommandCount by the driver). The index
// buffer token takes the DXGI index format, as D3D12_INDEX_BUFFER_VIEW holds it.
// Scratch (the copy and the preprocess buffer) is retired with the submission, as vulkan_scratch.h does.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include "command_list.h"
#include "indirect_patch_spv.h"
#include "vulkan_scratch.h"

namespace leonida {

// What replay needs to run one ExecuteIndirect with a multi-argument signature (IndirectExpander::prepare).
struct IndirectPlan {
    static constexpr std::uint32_t max_patches = 17;
    VkIndirectCommandsLayoutEXT layout = VK_NULL_HANDLE;
    VkShaderStageFlags stages = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDeviceAddress sequences = 0;  // the records the layout reads
    VkDeviceSize sequences_size = 0;
    VkDeviceAddress preprocess = 0;
    VkDeviceSize preprocess_size = 0;
    VkDeviceAddress count_address = 0;  // 0: max_count sequences
    std::uint32_t max_count = 0;
    // The pass that rebases buffer views (patch_count == 0: none).
    VkDeviceAddress patch_source = 0;
    std::uint32_t patch_dwords = 0, patch_stride_dwords = 0, patch_count = 0;
    std::uint32_t patch_offsets[max_patches]{};
    std::uint64_t heap_base = 0;
};

class IndirectExpander {
public:
    struct Properties {
        std::uint32_t max_stride = 0, max_sequences = 0, max_tokens = 0, max_token_offset = 0;
        VkIndirectCommandsInputModeFlagsEXT input_modes = 0;
        VkShaderStageFlags stages = 0;
    };

    // What a device creator needs to turn the extension on: query() reads the GPU (the extensions it lists, its
    // features and limits), then the extension names and the two enabled feature structs (the extension and
    // VK_KHR_maintenance5, for the preprocess buffer's usage flag) go into the device's create info.
    struct Support {
        bool available = false;
        Properties properties;
        VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT};
        VkPhysicalDeviceMaintenance5Features maintenance5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES};

        Support() = default;
        Support(const Support&) = delete;
        Support& operator=(const Support&) = delete;

        bool query(PFN_vkGetPhysicalDeviceFeatures2 get_features, PFN_vkGetPhysicalDeviceProperties2 get_properties,
                   VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& extensions) {
            const auto has = [&](const char* name) {
                return std::any_of(extensions.begin(), extensions.end(),
                                   [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
            };
            available = false;
            if (!get_features || !get_properties || !has(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME) ||
                !has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
                return false;
            VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT};
            VkPhysicalDeviceMaintenance5Features m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES, &dgc};
            VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &m5};
            get_features(physical, &features2);
            VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT dgc_properties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dgc_properties};
            get_properties(physical, &properties2);
            if (!dgc.deviceGeneratedCommands || !m5.maintenance5) return false;
            properties.max_stride = dgc_properties.maxIndirectCommandsIndirectStride;
            properties.max_sequences = dgc_properties.maxIndirectSequenceCount;
            properties.max_tokens = dgc_properties.maxIndirectCommandsTokenCount;
            properties.max_token_offset = dgc_properties.maxIndirectCommandsTokenOffset;
            properties.input_modes = dgc_properties.supportedIndirectCommandsInputModes;
            properties.stages = dgc_properties.supportedIndirectCommandsShaderStages;
            features.deviceGeneratedCommands = VK_TRUE;
            maintenance5.maintenance5 = VK_TRUE;
            return available = true;
        }
        // Puts the feature structs in front of the chain `head` points at.
        void chain(const void*& head) {
            maintenance5.pNext = const_cast<void*>(head);
            features.pNext = &maintenance5;
            head = &features;
        }
        static void add_extensions(std::vector<const char*>& names) {
            names.push_back(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
            names.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        }
    };

    // `allocate` returns device memory with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT.
    IndirectExpander(PFN_vkGetDeviceProcAddr get, VkDevice device, const Properties& properties,
                     ScratchBuffers::Allocate allocate)
        : get_(get), device_(device), properties_(properties), allocate_(std::move(allocate)) {
        loaded_ = load();
        if (!loaded_) return;
        ScratchBuffers::Config config;
        config.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        copies_ = std::make_unique<ScratchBuffers>(get_, device_, allocate_, config);
        loaded_ = copies_->loaded();
    }

    ~IndirectExpander() {
        if (!get_ || !device_) return;
        if (fn_.vkDeviceWaitIdle) fn_.vkDeviceWaitIdle(device_);
        for (auto& entry : layouts_)
            if (entry.second.layout) fn_.vkDestroyIndirectCommandsLayoutEXT(device_, entry.second.layout, nullptr);
        if (patch_pipeline_) fn_.vkDestroyPipeline(device_, patch_pipeline_, nullptr);
        if (patch_layout_) fn_.vkDestroyPipelineLayout(device_, patch_layout_, nullptr);
    }
    IndirectExpander(const IndirectExpander&) = delete;
    IndirectExpander& operator=(const IndirectExpander&) = delete;

    bool loaded() const { return loaded_; }

    struct Request {
        const std::shared_ptr<const IndirectSignature>* signature = nullptr;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkShaderStageFlags push_stages = 0;  // the stages of the layout's push-constant range
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkBuffer argument_buffer = VK_NULL_HANDLE;
        VkDeviceSize argument_offset = 0;  // in argument_buffer
        VkBuffer count_buffer = VK_NULL_HANDLE;
        VkDeviceSize count_offset = 0;
        std::uint32_t max_count = 0;
        std::uint64_t heap_base = 0;
    };

    // Makes (or finds) the layout for the signature and pipeline layout, and the scratch for the call. False when the
    // GPU cannot run it (a limit, a token, a stage) or scratch could not be had.
    bool prepare(const Request& r, IndirectPlan& out) {
        if (!loaded_ || !r.signature || !*r.signature || !r.max_count) return false;
        const IndirectSignature& sig = **r.signature;
        const bool compute = sig.kind == IndirectKind::dispatch;
        const VkShaderStageFlags stages = compute ? VK_SHADER_STAGE_COMPUTE_BIT : r.push_stages;
        if ((stages & ~properties_.stages) || sig.stride > properties_.max_stride ||
            r.max_count > properties_.max_sequences || sig.arguments.size() > properties_.max_tokens)
            return false;
        if (!compute && !(stages & VK_SHADER_STAGE_VERTEX_BIT)) return false;
        const VkDeviceSize span = VkDeviceSize(r.max_count - 1) * sig.stride + sig.arguments_size;
        std::lock_guard lock(mutex_);
        out = {};
        out.stages = stages;
        out.pipeline = r.pipeline;
        out.max_count = r.max_count;
        out.heap_base = r.heap_base;
        out.sequences_size = span;
        const VkDeviceAddress arguments = address(r.argument_buffer) + r.argument_offset;
        if (!arguments) return false;
        if (r.count_buffer) out.count_address = address(r.count_buffer) + r.count_offset;
        out.layout = layout_for(*r.signature, r.pipeline_layout, stages, compute);
        if (!out.layout) return false;
        // Buffer views: rebase them in a scratch copy.
        for (const IndirectArgument& a : sig.arguments) {
            if (a.type != IndirectArgumentType::vertex_buffer && a.type != IndirectArgumentType::index_buffer) continue;
            if (out.patch_count == IndirectPlan::max_patches) return false;
            out.patch_offsets[out.patch_count++] = a.offset / 4;
        }
        if (out.patch_count) {
            // Exactly the bytes the records occupy: the last record's tail past its arguments need not exist in the
            // application's buffer (D3D12 only asks for (n - 1) * stride + the arguments), and the sequences stay
            // `span` bytes long as in the unpatched case.
            const VkDeviceSize dwords = span / 4;
            if (dwords > UINT32_MAX / 2) return false;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            if (!copies_->get(span, buffer, offset)) return false;
            out.patch_source = arguments;
            out.sequences = copies_->address(buffer, offset);
            out.patch_dwords = std::uint32_t(dwords);
            out.patch_stride_dwords = sig.stride / 4;
            if (!patch_ready()) return false;
        } else {
            out.sequences = arguments;
        }
        // The preprocess buffer.
        VkGeneratedCommandsPipelineInfoEXT pipeline{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT};
        pipeline.pipeline = r.pipeline;
        VkGeneratedCommandsMemoryRequirementsInfoEXT info{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT,
                                                          &pipeline};
        info.indirectCommandsLayout = out.layout;
        info.maxSequenceCount = r.max_count;
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        fn_.vkGetGeneratedCommandsMemoryRequirementsEXT(device_, &info, &requirements);
        out.preprocess_size = requirements.memoryRequirements.size;
        if (out.preprocess_size) {
            // One pool per set of memory types the requirements allow, and room to align the start (a pool hands out
            // 256-byte aligned chunks).
            const std::uint32_t bits = requirements.memoryRequirements.memoryTypeBits;
            auto& pool = preprocess_[bits];
            if (!pool) {
                ScratchBuffers::Config config;
                config.usage = 0;
                config.usage2 = VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;
                config.memory_type_mask = bits;
                pool = std::make_unique<ScratchBuffers>(get_, device_, allocate_, config);
                if (!pool->loaded()) {
                    preprocess_.erase(bits);
                    return false;
                }
            }
            const VkDeviceSize alignment = std::max<VkDeviceSize>(1, requirements.memoryRequirements.alignment);
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            if (!pool->get(out.preprocess_size + (alignment > 256 ? alignment : 0), buffer, offset)) return false;
            const VkDeviceAddress start = pool->address(buffer, offset);
            out.preprocess = (start + alignment - 1) / alignment * alignment;
        }
        return true;
    }

    // Outside rendering: the pass that rebases buffer views (a no-op for a plan without any), ordered after whatever
    // wrote the records and before the commands that read the copy.
    void emit_prepass(VkCommandBuffer cb, const IndirectPlan& plan) const {
        if (!plan.patch_count) return;
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        fn_.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &before, 0, nullptr, 0, nullptr);
        struct Push {
            std::uint64_t source, destination, heap_base;
            std::uint32_t dwords, stride_dwords, patch_count, offsets[IndirectPlan::max_patches];
        } push{plan.patch_source, plan.sequences, plan.heap_base, plan.patch_dwords, plan.patch_stride_dwords,
               plan.patch_count, {}};
        std::copy(std::begin(plan.patch_offsets), std::end(plan.patch_offsets), push.offsets);
        fn_.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, patch_pipeline_);
        fn_.vkCmdPushConstants(cb, patch_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        // A 2D grid, rows of 65535 groups (the least maxComputeWorkGroupCount a device has): the shader numbers its
        // invocations row by row, so any record count fits.
        const std::uint32_t groups = (plan.patch_dwords + 63) / 64, columns = std::min(groups, 65535u);
        fn_.vkCmdDispatch(cb, columns, (groups + columns - 1) / columns, 1);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        fn_.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                 &after, 0, nullptr, 0, nullptr);
    }

    // Inside rendering for a draw layout, outside it for a dispatch layout, with the pipeline and push constants bound.
    void emit_execute(VkCommandBuffer cb, const IndirectPlan& plan) const {
        VkGeneratedCommandsPipelineInfoEXT pipeline{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT};
        pipeline.pipeline = plan.pipeline;
        VkGeneratedCommandsInfoEXT info{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT, &pipeline};
        info.shaderStages = plan.stages;
        info.indirectCommandsLayout = plan.layout;
        info.indirectAddress = plan.sequences;
        info.indirectAddressSize = plan.sequences_size;
        info.preprocessAddress = plan.preprocess;
        info.preprocessSize = plan.preprocess_size;
        info.maxSequenceCount = plan.max_count;
        info.sequenceCountAddress = plan.count_address;
        fn_.vkCmdExecuteGeneratedCommandsEXT(cb, VK_FALSE, &info);
    }

    // Scratch life cycle, as ScratchBuffers: collect() before the first get() of a submission, retire() after it.
    void collect() {
        std::lock_guard lock(mutex_);
        copies_->collect();
        for (auto& pool : preprocess_) pool.second->collect();
    }
    // The layouts this submission used are in use until its ticket completes (layout_for evicts only past that).
    void retire(VkSemaphore timeline, std::uint64_t ticket) {
        std::lock_guard lock(mutex_);
        copies_->retire(timeline, ticket);
        for (auto& pool : preprocess_) pool.second->retire(timeline, ticket);
        if (ticket)
            for (Entry* entry : current_) entry->stamps.push_back({timeline, ticket});
        current_.clear();
    }
    void forget(VkSemaphore timeline) {
        std::lock_guard lock(mutex_);
        copies_->forget(timeline);
        for (auto& pool : preprocess_) pool.second->forget(timeline);
        for (auto& entry : layouts_)
            entry.second.stamps.erase(std::remove_if(entry.second.stamps.begin(), entry.second.stamps.end(),
                                                     [&](const auto& stamp) { return stamp.first == timeline; }),
                                      entry.second.stamps.end());
    }

private:
    struct Functions {
#define LEONIDA_INDIRECT_FUNCTIONS(X)                                                                              \
    X(vkCreateIndirectCommandsLayoutEXT) X(vkDestroyIndirectCommandsLayoutEXT)                                     \
    X(vkGetGeneratedCommandsMemoryRequirementsEXT) X(vkCmdExecuteGeneratedCommandsEXT)                             \
    X(vkGetBufferDeviceAddress) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout)         \
    X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCmdPipelineBarrier)            \
    X(vkCmdBindPipeline) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkDeviceWaitIdle) X(vkGetSemaphoreCounterValue)
#define LEONIDA_INDIRECT_MEMBER(name) PFN_##name name = nullptr;
        LEONIDA_INDIRECT_FUNCTIONS(LEONIDA_INDIRECT_MEMBER)
#undef LEONIDA_INDIRECT_MEMBER
    };

    bool load() {
        bool all = true;
#define LEONIDA_INDIRECT_LOAD(name)                                  \
    fn_.name = reinterpret_cast<PFN_##name>(get_(device_, #name));  \
    all = all && fn_.name != nullptr;
        LEONIDA_INDIRECT_FUNCTIONS(LEONIDA_INDIRECT_LOAD)
#undef LEONIDA_INDIRECT_LOAD
#undef LEONIDA_INDIRECT_FUNCTIONS
        return all;
    }

    VkDeviceAddress address(VkBuffer buffer) const {
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = buffer;
        return fn_.vkGetBufferDeviceAddress(device_, &info);
    }

    // Layouts are shared by signatures that describe the same records (the key is the description, not the object).
    // At most kMaxLayouts are kept: past that the least recently used one that no submission still running or being
    // prepared uses is destroyed, and when there is none the call is refused.
    static constexpr std::size_t kMaxLayouts = 64;
    struct Entry {
        VkIndirectCommandsLayoutEXT layout = VK_NULL_HANDLE;
        std::uint64_t last_use = 0;
        std::vector<std::pair<VkSemaphore, std::uint64_t>> stamps;  // submissions that used it: timeline, ticket
    };
    using Key = std::vector<std::uint64_t>;

    static Key key_of(const IndirectSignature& sig, VkPipelineLayout pipeline_layout, VkShaderStageFlags stages) {
        Key key{reinterpret_cast<std::uintptr_t>(pipeline_layout), stages, sig.stride};
        for (const IndirectArgument& a : sig.arguments)
            for (const std::uint64_t v : {std::uint64_t(a.type), std::uint64_t(a.offset), std::uint64_t(a.user_dword),
                                          std::uint64_t(a.dwords), std::uint64_t(a.slot)})
                key.push_back(v);
        return key;
    }

    bool evict_one() {
        Entry* best = nullptr;
        Key best_key;
        for (auto& [key, entry] : layouts_) {
            if (std::find(current_.begin(), current_.end(), &entry) != current_.end()) continue;
            bool busy = false;
            for (auto& stamp : entry.stamps) {
                std::uint64_t done = 0;
                if (fn_.vkGetSemaphoreCounterValue(device_, stamp.first, &done) != VK_SUCCESS || done < stamp.second)
                    busy = true;
            }
            if (busy || (best && entry.last_use >= best->last_use)) continue;
            best = &entry;
            best_key = key;
        }
        if (!best) return false;
        fn_.vkDestroyIndirectCommandsLayoutEXT(device_, best->layout, nullptr);
        layouts_.erase(best_key);
        return true;
    }

    // One token per argument, in record order (the action last, as the layout rules want), at its byte offset.
    VkIndirectCommandsLayoutEXT layout_for(const std::shared_ptr<const IndirectSignature>& signature,
                                           VkPipelineLayout pipeline_layout, VkShaderStageFlags stages, bool compute) {
        const IndirectSignature& sig = *signature;
        const Key key = key_of(sig, pipeline_layout, stages);
        if (const auto found = layouts_.find(key); found != layouts_.end()) {
            found->second.last_use = ++clock_;
            if (std::find(current_.begin(), current_.end(), &found->second) == current_.end())
                current_.push_back(&found->second);
            return found->second.layout;
        }
        if (layouts_.size() >= kMaxLayouts && !evict_one()) return VK_NULL_HANDLE;
        std::vector<VkIndirectCommandsLayoutTokenEXT> tokens;
        std::vector<VkIndirectCommandsPushConstantTokenEXT> pushes(sig.arguments.size());
        std::vector<VkIndirectCommandsVertexBufferTokenEXT> vertices(sig.arguments.size());
        std::vector<VkIndirectCommandsIndexBufferTokenEXT> indices(sig.arguments.size());
        for (std::size_t i = 0; i < sig.arguments.size(); ++i) {
            const IndirectArgument& a = sig.arguments[i];
            VkIndirectCommandsLayoutTokenEXT token{VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT};
            token.offset = a.offset;
            if (a.offset > properties_.max_token_offset) return VK_NULL_HANDLE;
            switch (a.type) {
            case IndirectArgumentType::constants:
            case IndirectArgumentType::root_view:
                pushes[i].updateRange = {stages, a.user_dword * 4, a.dwords * 4};
                token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT;
                token.data.pPushConstant = &pushes[i];
                break;
            case IndirectArgumentType::vertex_buffer:
                vertices[i].vertexBindingUnit = a.slot;
                token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_VERTEX_BUFFER_EXT;
                token.data.pVertexBuffer = &vertices[i];
                break;
            case IndirectArgumentType::index_buffer:
                if (!(properties_.input_modes & VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT))
                    return VK_NULL_HANDLE;
                indices[i].mode = VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT;
                token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT;
                token.data.pIndexBuffer = &indices[i];
                break;
            case IndirectArgumentType::draw: token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT; break;
            case IndirectArgumentType::draw_indexed: token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT; break;
            case IndirectArgumentType::dispatch: token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT; break;
            }
            tokens.push_back(token);
        }
        VkIndirectCommandsLayoutCreateInfoEXT info{VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT};
        info.shaderStages = stages;
        info.indirectStride = sig.stride;
        info.pipelineLayout = pipeline_layout;
        info.tokenCount = std::uint32_t(tokens.size());
        info.pTokens = tokens.data();
        VkIndirectCommandsLayoutEXT layout = VK_NULL_HANDLE;
        if (fn_.vkCreateIndirectCommandsLayoutEXT(device_, &info, nullptr, &layout) != VK_SUCCESS) return VK_NULL_HANDLE;
        (void)compute;
        Entry& entry = layouts_[key];
        entry.layout = layout;
        entry.last_use = ++clock_;
        current_.push_back(&entry);
        return layout;
    }

    bool patch_ready() {
        if (patch_pipeline_) return true;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkShaderModule shader = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 * 3 + 4 * 3 + 4 * IndirectPlan::max_patches};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        bool ok = fn_.vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout) == VK_SUCCESS;
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = sizeof(indirect_patch_spirv);
        module.pCode = indirect_patch_spirv;
        ok = ok && fn_.vkCreateShaderModule(device_, &module, nullptr, &shader) == VK_SUCCESS;
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = shader;
        info.stage.pName = "main";
        info.layout = layout;
        ok = ok && fn_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS;
        if (shader) fn_.vkDestroyShaderModule(device_, shader, nullptr);
        if (!ok) {
            if (layout) fn_.vkDestroyPipelineLayout(device_, layout, nullptr);
            return false;
        }
        patch_layout_ = layout;
        patch_pipeline_ = pipeline;
        return true;
    }

    PFN_vkGetDeviceProcAddr get_;
    VkDevice device_;
    Properties properties_;
    ScratchBuffers::Allocate allocate_;
    Functions fn_;
    bool loaded_ = false;
    std::uint64_t clock_ = 0;
    std::vector<Entry*> current_;  // layouts the submission being prepared uses
    std::mutex mutex_;
    std::map<Key, Entry> layouts_;
    std::unique_ptr<ScratchBuffers> copies_;
    std::map<std::uint32_t, std::unique_ptr<ScratchBuffers>> preprocess_;  // by allowed memory types
    VkPipelineLayout patch_layout_ = VK_NULL_HANDLE;
    VkPipeline patch_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace leonida
