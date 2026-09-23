#include "engine/render/vulkan/VulkanContext.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/platform/Window.h"

namespace gdl {

namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
constexpr std::array<const char*, 1> kRequiredDeviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*userData*/) {
    const char* message = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        log::error("[vulkan] {}", message);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        log::warn("[vulkan] {}", message);
    } else {
        log::trace("[vulkan] {}", message);
    }
    return VK_FALSE;
}

bool hasLayer(std::span<const VkLayerProperties> layers, std::string_view name) {
    return std::ranges::any_of(layers, [name](const VkLayerProperties& layer) {
        return std::string_view(layer.layerName) == name;
    });
}

bool hasExtension(std::span<const VkExtensionProperties> extensions, std::string_view name) {
    return std::ranges::any_of(extensions, [name](const VkExtensionProperties& extension) {
        return std::string_view(extension.extensionName) == name;
    });
}

const char* deviceTypeName(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

} // namespace

VulkanContext::VulkanContext(Window& window, bool enableValidation) {
    createInstance(window, enableValidation);
    if (m_validationEnabled) {
        createDebugMessenger();
    }
    GDL_VERIFY(window.createVulkanSurface(m_instance, &m_surface),
               "Failed to create a Vulkan presentation surface for the window");
    pickPhysicalDevice();
    createDevice();
    createAllocator();
    createUploadPool();
    m_depthFormat = findDepthFormat();
}

VulkanContext::~VulkanContext() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }
    if (m_uploadPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_uploadPool, nullptr);
    }
    if (m_allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_allocator);
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    }
    if (m_debugMessenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }
}

void VulkanContext::createInstance(Window& window, bool enableValidation) {
    GDL_VERIFY(volkInitialize() == VK_SUCCESS,
               "Vulkan loader not found. Install a Vulkan-capable graphics driver.");

    const std::uint32_t loaderVersion = volkGetInstanceVersion();
    log::info("Vulkan loader: {}.{}.{}", VK_API_VERSION_MAJOR(loaderVersion),
              VK_API_VERSION_MINOR(loaderVersion), VK_API_VERSION_PATCH(loaderVersion));
    GDL_VERIFY(loaderVersion >= VK_API_VERSION_1_3,
               "Vulkan 1.3 or newer is required (update your graphics driver)");

    std::uint32_t layerCount = 0;
    GDL_VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));
    std::vector<VkLayerProperties> layers(layerCount);
    GDL_VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()));

    std::uint32_t extensionCount = 0;
    GDL_VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    GDL_VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
                                                        availableExtensions.data()));

    std::vector<const char*> extensions = window.requiredVulkanInstanceExtensions();
    std::vector<const char*> enabledLayers;

    if (enableValidation) {
        if (hasLayer(layers, kValidationLayerName) &&
            hasExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            enabledLayers.push_back(kValidationLayerName);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            m_validationEnabled = true;
            log::info("Vulkan validation layer enabled");
        } else {
            log::warn("Vulkan validation requested but {} is not installed; continuing without it",
                      kValidationLayerName);
        }
    }

    for (const char* extension : extensions) {
        GDL_VERIFY(hasExtension(availableExtensions, extension),
                   "A required Vulkan instance extension is missing");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Gauntlet Dark Legacy";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "gdl";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledLayerCount = static_cast<std::uint32_t>(enabledLayers.size());
    createInfo.ppEnabledLayerNames = enabledLayers.data();
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    GDL_VK_CHECK(vkCreateInstance(&createInfo, nullptr, &m_instance));
    volkLoadInstance(m_instance);
}

void VulkanContext::createDebugMessenger() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugMessengerCallback;
    GDL_VK_CHECK(
        vkCreateDebugUtilsMessengerEXT(m_instance, &createInfo, nullptr, &m_debugMessenger));
}

void VulkanContext::pickPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    GDL_VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr));
    GDL_VERIFY(deviceCount > 0, "No Vulkan-capable GPU found");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    GDL_VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data()));

    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);

        if (properties.apiVersion < VK_API_VERSION_1_3) {
            log::trace("Skipping {}: Vulkan {}.{} < 1.3", properties.deviceName,
                       VK_API_VERSION_MAJOR(properties.apiVersion),
                       VK_API_VERSION_MINOR(properties.apiVersion));
            continue;
        }

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        std::uint32_t graphicsFamily = VK_QUEUE_FAMILY_IGNORED;
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 presentSupported = VK_FALSE;
            GDL_VK_CHECK(
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, m_surface, &presentSupported));
            if (presentSupported == VK_TRUE) {
                graphicsFamily = i;
                break;
            }
        }
        if (graphicsFamily == VK_QUEUE_FAMILY_IGNORED) {
            continue;
        }

        std::uint32_t extensionCount = 0;
        GDL_VK_CHECK(
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr));
        std::vector<VkExtensionProperties> extensions(extensionCount);
        GDL_VK_CHECK(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount,
                                                          extensions.data()));
        const bool hasAllExtensions =
            std::ranges::all_of(kRequiredDeviceExtensions, [&extensions](const char* required) {
                return hasExtension(extensions, required);
            });
        if (!hasAllExtensions) {
            continue;
        }

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(candidate, &features2);
        if (features13.dynamicRendering == VK_FALSE || features13.synchronization2 == VK_FALSE) {
            continue;
        }

        int score = 10;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score = 1000;
        } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score = 100;
        }

        if (score > bestScore) {
            bestScore = score;
            m_physicalDevice = candidate;
            m_properties = properties;
            m_graphicsQueueFamily = graphicsFamily;
        }
    }

    GDL_VERIFY(m_physicalDevice != VK_NULL_HANDLE,
               "No suitable GPU: need Vulkan 1.3 with dynamicRendering, synchronization2 and "
               "a graphics queue that can present");

    log::info("GPU: {} ({}, Vulkan {}.{}.{}, driver 0x{:08x})", m_properties.deviceName,
              deviceTypeName(m_properties.deviceType),
              VK_API_VERSION_MAJOR(m_properties.apiVersion),
              VK_API_VERSION_MINOR(m_properties.apiVersion),
              VK_API_VERSION_PATCH(m_properties.apiVersion), m_properties.driverVersion);
}

void VulkanContext::createDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features13;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features2;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(kRequiredDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions.data();

    GDL_VK_CHECK(vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device));
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
}

void VulkanContext::createAllocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo createInfo{};
    createInfo.physicalDevice = m_physicalDevice;
    createInfo.device = m_device;
    createInfo.instance = m_instance;
    createInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    createInfo.pVulkanFunctions = &functions;

    GDL_VK_CHECK(vmaCreateAllocator(&createInfo, &m_allocator));
}

void VulkanContext::createUploadPool() {
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    createInfo.queueFamilyIndex = m_graphicsQueueFamily;
    GDL_VK_CHECK(vkCreateCommandPool(m_device, &createInfo, nullptr, &m_uploadPool));
}

VkFormat VulkanContext::findDepthFormat() const {
    constexpr std::array<VkFormat, 3> kCandidates{
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (const VkFormat format : kCandidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) !=
            0) {
            return format;
        }
    }
    GDL_FATAL("No supported depth buffer format");
}

VkCommandBuffer VulkanContext::beginOneShotCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_uploadPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    GDL_VK_CHECK(vkAllocateCommandBuffers(m_device, &allocInfo, &cmd));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    GDL_VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));
    return cmd;
}

void VulkanContext::endOneShotCommands(VkCommandBuffer cmd) {
    GDL_VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    GDL_VK_CHECK(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
    GDL_VK_CHECK(vkQueueWaitIdle(m_graphicsQueue));

    vkFreeCommandBuffers(m_device, m_uploadPool, 1, &cmd);
}

void VulkanContext::waitIdle() {
    GDL_VK_CHECK(vkDeviceWaitIdle(m_device));
}

} // namespace gdl
