#pragma once

#include <span>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/vulkan/VulkanCommon.h"

namespace gdl {

class VulkanContext;

/** RGBA8 sampled image plus the descriptor set that binds it. */
class VulkanTexture final : public Texture {
public:
    VulkanTexture(VulkanContext& context, VkDescriptorPool descriptorPool,
                  VkDescriptorSetLayout setLayout, VkSampler sampler, const TextureDesc& desc,
                  std::span<const u8> rgba8Pixels);
    ~VulkanTexture() override;

    GDL_NON_COPYABLE_NON_MOVABLE(VulkanTexture);

    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }

    VkImage image() const { return m_image; }
    VkDescriptorSet descriptorSet() const { return m_descriptorSet; }

private:
    VulkanContext& m_context;
    VkDescriptorPool m_descriptorPool;
    u32 m_width;
    u32 m_height;

    VkImage m_image = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
};

} // namespace gdl
