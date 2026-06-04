// SPDX-License-Identifier: MPL-2.0

#include "VulkanDispatch.h"

#ifdef XR_USE_GRAPHICS_API_VULKAN

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

PFN_vkGetInstanceProcAddr ResolveVulkanGetInstanceProcAddrFromProcess(
    const VulkanDispatch& dispatch)
{
    if (dispatch.getInstanceProcAddr != nullptr)
    {
        return dispatch.getInstanceProcAddr;
    }

#if defined(_WIN32)
    HMODULE vulkanModule = GetModuleHandleW(L"vulkan-1.dll");
    if (vulkanModule == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(vulkanModule, "vkGetInstanceProcAddr"));
#else
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
#endif
}

#endif // XR_USE_GRAPHICS_API_VULKAN
