#pragma once

#include <filesystem>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/vulkan/VulkanCommon.h"

namespace gdl {

class VulkanContext;

/** Graphics pipeline for ImmediateVertex geometry: one blend mode, reversed-Z depth, one
 * texture. Additive pipelines leave the depth buffer alone. */
class VulkanPipeline {
public:
    enum class Effect : u8 { None, DepthOfField, Bloom, AmbientOcclusion };
    VulkanPipeline(VulkanContext& context, const std::filesystem::path& shaderDirectory,
                   VkFormat colorFormat, VkFormat depthFormat,
                   VkDescriptorSetLayout textureSetLayout, BlendMode blend,
                   VkSampleCountFlagBits samples, Effect effect = Effect::None,
                   VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE,
                   bool alphaToCoverage = false);
    ~VulkanPipeline();

    GDL_NON_COPYABLE_NON_MOVABLE(VulkanPipeline);

    VkPipeline handle() const { return m_pipeline; }
    VkPipelineLayout layout() const { return m_layout; }

    /** What every draw pushes: its transform, then the coordinate offset, alpha test and
     * darkening, then the coordinate scale, second-stage mode and RGB combiner scale.
     * The stage mode is 1 for a masked skin, 0 for a lightmap, or a negative frame blend. */
    struct PushConstants {
        Mat4 transform;
        Vec4 params;
        Vec4 scale;
        Vec4 heatDepths{0.0f};
        Vec4 heatTimes{0.0f};
    };
    static constexpr u32 kPushConstantSize = sizeof(PushConstants);
    static_assert(kPushConstantSize <= 128, "Stay within Vulkan's minimum push-constant capacity");

private:
    VkShaderModule loadShaderModule(const std::filesystem::path& path) const;

    VulkanContext& m_context;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace gdl
