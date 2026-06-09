// SPDX-License-Identifier: MPL-2.0

#pragma once

#ifdef XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>

// Global Vulkan dispatch — all Vulkan calls go through function pointers
// resolved from the app's pfnGetInstanceProcAddr. This avoids loading the
// system Vulkan loader/MoltenVK which would conflict with app-embedded MoltenVK.

struct VulkanDispatch
{
    // The app's entry point — set by OxrCreateVulkanInstanceKHR (v2 path)
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;

    // Instance-level functions (resolved after VkInstance creation)
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;

    // Pre-instance functions (resolved from pfnGetInstanceProcAddr with VK_NULL_HANDLE)
    PFN_vkCreateInstance createInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties = nullptr;

    // The VkInstance created on behalf of the app
    VkInstance instance = VK_NULL_HANDLE;

    void LoadPreInstanceFunctions()
    {
        if (!getInstanceProcAddr)
        {
            return;
        }
        createInstance = (PFN_vkCreateInstance)
            getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
        enumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)
            getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    }

    void LoadInstanceFunctions(VkInstance inst)
    {
        if (!getInstanceProcAddr || inst == VK_NULL_HANDLE)
        {
            return;
        }
        instance = inst;
        destroyInstance = (PFN_vkDestroyInstance)
            getInstanceProcAddr(inst, "vkDestroyInstance");
        enumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)
            getInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
        getPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)
            getInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties");
        getPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
            getInstanceProcAddr(inst, "vkGetPhysicalDeviceMemoryProperties");
        getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
            getInstanceProcAddr(inst, "vkGetDeviceProcAddr");
        createDevice = (PFN_vkCreateDevice)
            getInstanceProcAddr(inst, "vkCreateDevice");
        getDeviceQueue = (PFN_vkGetDeviceQueue)
            getInstanceProcAddr(inst, "vkGetDeviceQueue");
    }
};

// Global instance — set up during Vulkan session creation
extern VulkanDispatch gVulkanDispatch;

PFN_vkGetInstanceProcAddr ResolveVulkanGetInstanceProcAddrFromProcess(
    const VulkanDispatch& dispatch);

#endif // XR_USE_GRAPHICS_API_VULKAN
