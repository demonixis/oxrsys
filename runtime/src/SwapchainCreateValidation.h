// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "GraphicsTypes.h"

#include <openxr/openxr.h>

namespace oxrsys::swapchain
{

constexpr XrSwapchainCreateFlags SupportedCreateFlags =
    XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;

constexpr XrSwapchainUsageFlags SupportedUsageFlags(GraphicsApi api)
{
    XrSwapchainUsageFlags flags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
        XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    if (api == GraphicsApi::Metal)
    {
        // Metal blit destinations do not require an MTLTextureUsage flag. Blender
        // copies its rendered view into the OpenXR texture and declares that use.
        flags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        // Godot's Metal driver renders through a view of the swapchain texture with the
        // sibling sRGB/UNORM pixel format. SwapchainMetal maps this to
        // MTLTextureUsagePixelFormatView.
        flags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    }
    if (api == GraphicsApi::Vulkan)
    {
        // Godot creates UNORM swapchain images and renders through an sRGB view of them.
        // SwapchainVulkan maps this to VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT.
        flags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    }
    return flags;
}

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
        (createInfo.usageFlags & ~SupportedUsageFlags(api)) != 0)
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
    if (isDepth && (createInfo.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) != 0)
    {
        // Mutable format exists for UNORM color images viewed as sRGB; depth formats
        // have no compatible view format the runtime could offer.
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

} // namespace oxrsys::swapchain
