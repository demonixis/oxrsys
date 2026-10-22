// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "SwapchainCreateValidation.h"

TEST_CASE("Swapchain validation matches the formats enumerated per graphics API",
          "[graphics][swapchain]")
{
    constexpr int64_t metalFormats[] = {81, 80, 71, 70, 252, 260};
    constexpr int64_t vulkanFormats[] = {50, 44, 43, 37, 126, 130};
    for (const int64_t format : metalFormats)
    {
        CHECK(oxrsys::swapchain::IsSupportedFormat(GraphicsApi::Metal, format));
        CHECK_FALSE(oxrsys::swapchain::IsSupportedFormat(GraphicsApi::Vulkan, format));
    }
    for (const int64_t format : vulkanFormats)
    {
        CHECK(oxrsys::swapchain::IsSupportedFormat(GraphicsApi::Vulkan, format));
        CHECK_FALSE(oxrsys::swapchain::IsSupportedFormat(GraphicsApi::Metal, format));
    }
    CHECK_FALSE(oxrsys::swapchain::IsSupportedFormat(GraphicsApi::Vulkan, 9999));
}

TEST_CASE("Swapchain usage and creation flags are validated per graphics API",
          "[graphics][swapchain]")
{
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.format = 80;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) == XR_SUCCESS);

    createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) == XR_SUCCESS);
    createInfo.format = 37;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Vulkan, createInfo) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
    createInfo.format = 80;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.createFlags = XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
    createInfo.createFlags = 0;
    // Godot's Metal driver renders through an sRGB/UNORM view of the swapchain texture.
    createInfo.format = 71;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                            XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) == XR_SUCCESS);
    createInfo.format = 252;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
    // Godot creates UNORM images and renders through an sRGB view of them.
    createInfo.format = 37;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                            XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Vulkan, createInfo) == XR_SUCCESS);
    createInfo.format = 126;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Vulkan, createInfo) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
}

TEST_CASE("Swapchain attachment usage must match the selected format",
          "[graphics][swapchain]")
{
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};

    createInfo.format = 80;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) == XR_SUCCESS);

    createInfo.format = 252;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Metal, createInfo) == XR_SUCCESS);

    createInfo.format = 37;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Vulkan, createInfo) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
    createInfo.format = 126;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    CHECK(oxrsys::swapchain::ValidateCreateInfo(GraphicsApi::Vulkan, createInfo) ==
          XR_ERROR_FEATURE_UNSUPPORTED);
}
