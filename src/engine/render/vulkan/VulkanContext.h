#pragma once

#include "engine/core/Types.h"
#include "engine/render/vulkan/VulkanCommon.h"

namespace gdl {

class Window;

/** Instance, device, queue and allocator: the renderer state that outlives the swapchain. */
class VulkanContext {
public:
    VulkanContext(Window& window, bool enableValidation);
    ~VulkanContext();

    GDL_NON_COPYABLE_NON_MOVABLE(VulkanContext);

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkSurfaceKHR surface() const { return m_surface; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    u32 graphicsQueueFamily() const { return m_graphicsQueueFamily; }
    VmaAllocator allocator() const { return m_allocator; }
    const VkPhysicalDeviceProperties& properties() const { return m_properties; }
    VkFormat depthFormat() const { return m_depthFormat; }

    /** Begins a one-off command buffer; endOneShotCommands submits it and waits. */
    VkCommandBuffer beginOneShotCommands();
    void endOneShotCommands(VkCommandBuffer cmd);

    void waitIdle();

private:
    void createInstance(Window& window, bool enableValidation);
    void createDebugMessenger();
    void pickPhysicalDevice();
    void createDevice();
    void createAllocator();
    void createUploadPool();
    VkFormat findDepthFormat() const;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_properties{};
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    u32 m_graphicsQueueFamily = 0;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkCommandPool m_uploadPool = VK_NULL_HANDLE;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    bool m_validationEnabled = false;
};

} // namespace gdl
