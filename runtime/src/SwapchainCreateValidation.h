// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "GraphicsTypes.h"

#include <openxr/openxr.h>

namespace oxrsys::swapchain
{

constexpr XrSwapchainCreateFlags SupportedCreateFlags =
    XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
constexpr XrSwapchainUsageFlags SupportedUsageFlags =
    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
    XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
    XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

constexpr bool IsSupportedFormat(GraphicsApi api, int64_t format)
{
    if (api == GraphicsApi::Metal)
    {
        return format == 81 || format == 80 || format == 71 || format == 70 ||
               format == 252 || format == 260;
    }
    if (api == GraphicsApi::Vulkan)
    {
        return format == 50 || format == 44 || format == 43 || format == 37 ||
               format == 126 || format == 130;
    }
    return false;
}

constexpr bool IsDepthFormat(GraphicsApi api, int64_t format)
{
    if (api == GraphicsApi::Metal)
    {
        return format == 252 || format == 260;
    }
    if (api == GraphicsApi::Vulkan)
    {
        return format == 126 || format == 130;
    }
    return false;
}

constexpr XrResult ValidateCreateInfo(GraphicsApi api,
                                      const XrSwapchainCreateInfo& createInfo)
{
    if ((createInfo.createFlags & ~SupportedCreateFlags) != 0 ||
        (createInfo.usageFlags & ~SupportedUsageFlags) != 0)
    {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    if (!IsSupportedFormat(api, createInfo.format))
    {
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }
    const bool isDepth = IsDepthFormat(api, createInfo.format);
    if ((isDepth &&
         (createInfo.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0) ||
        (!isDepth &&
         (createInfo.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0))
    {
        // The native backends create an attachment usage matching the format;
        // accepting the opposite attachment bit would promise capabilities the
        // returned MTLTexture/VkImage does not have.
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

} // namespace oxrsys::swapchain
