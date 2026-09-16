#pragma once

#include <filesystem>

#include "engine/core/Types.h"
#include "engine/render/vulkan/VulkanCommon.h"

namespace gdl {

class VulkanContext;

/** Graphics pipeline for ImmediateVertex geometry: alpha blend, reversed-Z depth, one texture. */
class VulkanPipeline {
public:
    VulkanPipeline(VulkanContext& context, const std::filesystem::path& shaderDirectory,
                   VkFormat colorFormat, VkFormat depthFormat,
                   VkDescriptorSetLayout textureSetLayout);
    ~VulkanPipeline();

    GDL_NON_COPYABLE_NON_MOVABLE(VulkanPipeline);

    VkPipeline handle() const { return m_pipeline; }
    VkPipelineLayout layout() const { return m_layout; }

    static constexpr u32 kPushConstantSize = sizeof(f32) * 16;

private:
    VkShaderModule loadShaderModule(const std::filesystem::path& path) const;

    VulkanContext& m_context;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace gdl
