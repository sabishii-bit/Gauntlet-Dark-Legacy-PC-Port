#pragma once

#include <vector>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/vulkan/VulkanCommon.h"

namespace gdl {

class VulkanContext;

/** Presentable images plus a matching depth buffer; rebuilt on resize. */
class VulkanSwapchain {
public:
    VulkanSwapchain(VulkanContext& context, Extent2D windowExtent, bool vsync);
    ~VulkanSwapchain();

    GDL_NON_COPYABLE_NON_MOVABLE(VulkanSwapchain);

    /** Waits for the device to idle and rebuilds at the new size. */
    void recreate(Extent2D windowExtent);

    VkResult acquireNextImage(VkSemaphore signalSemaphore, u32* imageIndex);
    VkResult present(VkSemaphore waitSemaphore, u32 imageIndex);

    VkFormat colorFormat() const { return m_colorFormat; }
    VkFormat depthFormat() const { return m_depthFormat; }
    VkExtent2D extent() const { return m_extent; }
    u32 imageCount() const { return static_cast<u32>(m_images.size()); }
    VkImage image(u32 index) const { return m_images[index]; }
    VkImageView imageView(u32 index) const { return m_imageViews[index]; }
    VkImage depthImage() const { return m_depthImage; }
    VkImageView depthImageView() const { return m_depthImageView; }

private:
    void create(Extent2D windowExtent, VkSwapchainKHR oldSwapchain);
    void destroyImageResources();

    VulkanContext& m_context;
    bool m_vsync = true;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D m_extent{};
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;

    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    VkImage m_depthImage = VK_NULL_HANDLE;
    VmaAllocation m_depthAllocation = VK_NULL_HANDLE;
    VkImageView m_depthImageView = VK_NULL_HANDLE;
};

} // namespace gdl
