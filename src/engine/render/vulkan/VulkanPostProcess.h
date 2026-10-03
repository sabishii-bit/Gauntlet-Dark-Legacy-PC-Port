#pragma once

#include <array>
#include <filesystem>
#include <memory>

#include "engine/core/SpecialMembers.h"
#include "engine/render/DepthOfField.h"
#include "engine/render/vulkan/VulkanCommon.h"
#include "engine/render/vulkan/VulkanPipeline.h"

namespace gdl {
class VulkanContext;
class VulkanSwapchain;

/** Optional scene-only pass. Resources live until resize; the device idles before destruction.
 * record runs outside dynamic rendering and returns attachments ready for LOAD. */
class VulkanPostProcess {
public:
    VulkanPostProcess(VulkanContext& context, const VulkanSwapchain& swapchain,
                      const std::filesystem::path& shaders, VkDescriptorSetLayout textureLayout,
                      VkSampler colorSampler, VkSampler depthSampler);
    ~VulkanPostProcess();
    GDL_NON_COPYABLE_NON_MOVABLE(VulkanPostProcess);

    void record(VkCommandBuffer cmd, const VulkanSwapchain& swapchain, u32 imageIndex,
                const DepthOfField& settings);
    void recordBloom(VkCommandBuffer cmd, const VulkanSwapchain& swapchain, u32 imageIndex);

private:
    void recordPass(VkCommandBuffer cmd, const VulkanSwapchain& swapchain, u32 imageIndex,
                    const VulkanPipeline& pipeline, const VulkanPipeline::PushConstants& constants);
    VulkanContext& m_context;
    std::unique_ptr<VulkanPipeline> m_pipeline;
    std::unique_ptr<VulkanPipeline> m_bloomPipeline;
    VkImage m_color = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> m_sets{};
    bool m_initialized = false;
};
} // namespace gdl
