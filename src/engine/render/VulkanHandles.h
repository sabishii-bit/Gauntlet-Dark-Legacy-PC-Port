#pragma once

// Handle typedefs matching <vulkan/vulkan_core.h> on 64-bit targets, so public
// headers can name them without including the Vulkan headers.

static_assert(sizeof(void*) == 8, "gdl targets 64-bit platforms only");

struct VkInstance_T;
using VkInstance = VkInstance_T*;

struct VkSurfaceKHR_T;
using VkSurfaceKHR = VkSurfaceKHR_T*;
