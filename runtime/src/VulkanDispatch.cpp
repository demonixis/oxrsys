// SPDX-License-Identifier: MPL-2.0

#include "VulkanDispatch.h"

#ifdef XR_USE_GRAPHICS_API_VULKAN

#include <dlfcn.h>

PFN_vkGetInstanceProcAddr ResolveVulkanGetInstanceProcAddrFromProcess(
    const VulkanDispatch& dispatch)
{
    if (dispatch.getInstanceProcAddr != nullptr)
    {
        return dispatch.getInstanceProcAddr;
    }

    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
}

#endif // XR_USE_GRAPHICS_API_VULKAN
