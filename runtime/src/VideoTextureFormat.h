// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

// Metal pixel-format values are part of the public API and are also the values
// returned by xrEnumerateSwapchainFormats. Keep this header independent of
// Objective-C so the decision contract remains unit-testable in C++.
namespace oxrsys::video
{

constexpr uint64_t MetalRgba8Unorm = 70;
constexpr uint64_t MetalRgba8UnormSrgb = 71;
constexpr uint64_t MetalBgra8Unorm = 80;
constexpr uint64_t MetalBgra8UnormSrgb = 81;

enum class TextureCopyMode
{
    DirectBgra,
    ComputeConversion,
    Unsupported,
};

constexpr TextureCopyMode SelectTextureCopyMode(uint64_t pixelFormat)
{
    switch (pixelFormat)
    {
        case MetalBgra8Unorm:
            return TextureCopyMode::DirectBgra;
        case MetalRgba8Unorm:
        case MetalRgba8UnormSrgb:
        case MetalBgra8UnormSrgb:
            return TextureCopyMode::ComputeConversion;
        default:
            return TextureCopyMode::Unsupported;
    }
}

constexpr bool IsSrgbTextureFormat(uint64_t pixelFormat)
{
    return pixelFormat == MetalRgba8UnormSrgb ||
           pixelFormat == MetalBgra8UnormSrgb;
}

} // namespace oxrsys::video
