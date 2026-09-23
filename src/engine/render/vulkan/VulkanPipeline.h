#pragma once

#include <filesystem>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/vulkan/VulkanCommon.h"

namespace gdl {

class VulkanContext;

/** Graphics pipeline for ImmediateVertex geometry: one blend mode, reversed-Z depth, one
 * texture. Additive pipelines leave the depth buffer alone. */
class VulkanPipeline {
public:
    VulkanPipeline(VulkanContext& context, const std::filesystem::path& shaderDirectory,
                   VkFormat colorFormat, VkFormat depthFormat,
                   VkDescriptorSetLayout textureSetLayout, BlendMode blend);
    ~VulkanPipeline();

    GDL_NON_COPYABLE_NON_MOVABLE(VulkanPipeline);

    VkPipeline handle() const { return m_pipeline; }
    VkPipelineLayout layout() const { return m_layout; }

    /** What every draw pushes: its transform, then the coordinate offset, alpha test and
     * darkening, then the coordinate scale and masked-texture mode. */
    struct PushConstants {
        Mat4 transform;
        Vec4 params;
        Vec4 scale;
    };
    static constexpr u32 kPushConstantSize = sizeof(PushConstants);

private:
    VkShaderModule loadShaderModule(const std::filesystem::path& path) const;

    VulkanContext& m_context;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace gdl
