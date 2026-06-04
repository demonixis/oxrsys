// SPDX-License-Identifier: MPL-2.0

#pragma once

#ifdef XR_USE_GRAPHICS_API_VULKAN

#include <vulkan/vulkan.h>

struct VulkanGraphicsContext
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    uint32_t queueIndex = 0;
};

#endif // XR_USE_GRAPHICS_API_VULKAN
