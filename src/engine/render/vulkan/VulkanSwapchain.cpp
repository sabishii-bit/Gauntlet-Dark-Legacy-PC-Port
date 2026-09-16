#include "engine/render/vulkan/VulkanSwapchain.h"

#include <algorithm>
#include <limits>

#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/render/vulkan/VulkanContext.h"

namespace gdl {

VulkanSwapchain::VulkanSwapchain(VulkanContext& context, Extent2D windowExtent, bool vsync)
    : m_context(context), m_vsync(vsync), m_depthFormat(context.depthFormat()) {
    create(windowExtent, VK_NULL_HANDLE);
}

VulkanSwapchain::~VulkanSwapchain() {
    destroyImageResources();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_context.device(), m_swapchain, nullptr);
    }
}

void VulkanSwapchain::recreate(Extent2D windowExtent) {
    m_context.waitIdle();
    const VkSwapchainKHR old = m_swapchain;
    destroyImageResources();
    create(windowExtent, old);
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_context.device(), old, nullptr);
    }
}

void VulkanSwapchain::create(Extent2D windowExtent, VkSwapchainKHR oldSwapchain) {
    const VkPhysicalDevice physicalDevice = m_context.physicalDevice();
    const VkSurfaceKHR surface = m_context.surface();
    const VkDevice device = m_context.device();

    VkSurfaceCapabilitiesKHR capabilities{};
    GDL_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities));

    u32 formatCount = 0;
    GDL_VK_CHECK(
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    GDL_VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount,
                                                      formats.data()));
    GDL_VERIFY(!formats.empty(), "Surface reports no formats");

    const auto preferred = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    const VkSurfaceFormatKHR chosenFormat =
        preferred != formats.end() ? *preferred : formats.front();
    m_colorFormat = chosenFormat.format;
    m_colorSpace = chosenFormat.colorSpace;

    u32 modeCount = 0;
    GDL_VK_CHECK(
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &modeCount, nullptr));
    std::vector<VkPresentModeKHR> modes(modeCount);
    GDL_VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &modeCount,
                                                           modes.data()));

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!m_vsync) {
        if (std::ranges::contains(modes, VK_PRESENT_MODE_MAILBOX_KHR)) {
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (std::ranges::contains(modes, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }

    if (capabilities.currentExtent.width != std::numeric_limits<u32>::max()) {
        m_extent = capabilities.currentExtent;
    } else {
        m_extent.width = std::clamp(windowExtent.width, capabilities.minImageExtent.width,
                                    capabilities.maxImageExtent.width);
        m_extent.height = std::clamp(windowExtent.height, capabilities.minImageExtent.height,
                                     capabilities.maxImageExtent.height);
    }
    GDL_VERIFY(m_extent.width > 0 && m_extent.height > 0, "Swapchain extent must be non-zero");

    u32 imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = m_colorFormat;
    createInfo.imageColorSpace = m_colorSpace;
    createInfo.imageExtent = m_extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    GDL_VK_CHECK(vkCreateSwapchainKHR(device, &createInfo, nullptr, &m_swapchain));

    u32 actualCount = 0;
    GDL_VK_CHECK(vkGetSwapchainImagesKHR(device, m_swapchain, &actualCount, nullptr));
    m_images.resize(actualCount);
    GDL_VK_CHECK(vkGetSwapchainImagesKHR(device, m_swapchain, &actualCount, m_images.data()));

    m_imageViews.resize(actualCount);
    for (u32 i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_colorFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        GDL_VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &m_imageViews[i]));
    }

    VkImageCreateInfo depthInfo{};
    depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthInfo.imageType = VK_IMAGE_TYPE_2D;
    depthInfo.format = m_depthFormat;
    depthInfo.extent = {m_extent.width, m_extent.height, 1};
    depthInfo.mipLevels = 1;
    depthInfo.arrayLayers = 1;
    depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo depthAlloc{};
    depthAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    depthAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    GDL_VK_CHECK(vmaCreateImage(m_context.allocator(), &depthInfo, &depthAlloc, &m_depthImage,
                                &m_depthAllocation, nullptr));

    VkImageViewCreateInfo depthView{};
    depthView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthView.image = m_depthImage;
    depthView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthView.format = m_depthFormat;
    depthView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthView.subresourceRange.levelCount = 1;
    depthView.subresourceRange.layerCount = 1;
    GDL_VK_CHECK(vkCreateImageView(device, &depthView, nullptr, &m_depthImageView));

    log::info("Swapchain: {}x{}, {} images, format {}, {}", m_extent.width, m_extent.height,
              actualCount, static_cast<int>(m_colorFormat),
              presentMode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : "no vsync");
}

void VulkanSwapchain::destroyImageResources() {
    const VkDevice device = m_context.device();
    if (m_depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_depthImageView, nullptr);
        m_depthImageView = VK_NULL_HANDLE;
    }
    if (m_depthImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_context.allocator(), m_depthImage, m_depthAllocation);
        m_depthImage = VK_NULL_HANDLE;
        m_depthAllocation = VK_NULL_HANDLE;
    }
    for (const VkImageView view : m_imageViews) {
        vkDestroyImageView(device, view, nullptr);
    }
    m_imageViews.clear();
    m_images.clear();
}

VkResult VulkanSwapchain::acquireNextImage(VkSemaphore signalSemaphore, u32* imageIndex) {
    return vkAcquireNextImageKHR(m_context.device(), m_swapchain, std::numeric_limits<u64>::max(),
                                 signalSemaphore, VK_NULL_HANDLE, imageIndex);
}

VkResult VulkanSwapchain::present(VkSemaphore waitSemaphore, u32 imageIndex) {
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &waitSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &imageIndex;
    return vkQueuePresentKHR(m_context.graphicsQueue(), &presentInfo);
}

} // namespace gdl
