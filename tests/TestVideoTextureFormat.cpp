// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "VideoTextureFormat.h"

using oxrsys::video::TextureCopyMode;

TEST_CASE("Every advertised Metal color format has an encoder copy path",
          "[video][format]")
{
    CHECK(oxrsys::video::SelectTextureCopyMode(oxrsys::video::MetalBgra8Unorm) ==
          TextureCopyMode::DirectBgra);
    CHECK(oxrsys::video::SelectTextureCopyMode(oxrsys::video::MetalBgra8UnormSrgb) ==
          TextureCopyMode::ComputeConversion);
    CHECK(oxrsys::video::SelectTextureCopyMode(oxrsys::video::MetalRgba8Unorm) ==
          TextureCopyMode::ComputeConversion);
    CHECK(oxrsys::video::SelectTextureCopyMode(oxrsys::video::MetalRgba8UnormSrgb) ==
          TextureCopyMode::ComputeConversion);
}

TEST_CASE("Unknown Metal color formats fail closed", "[video][format]")
{
    CHECK(oxrsys::video::SelectTextureCopyMode(0) == TextureCopyMode::Unsupported);
    CHECK_FALSE(oxrsys::video::IsSrgbTextureFormat(oxrsys::video::MetalRgba8Unorm));
    CHECK(oxrsys::video::IsSrgbTextureFormat(oxrsys::video::MetalRgba8UnormSrgb));
    CHECK(oxrsys::video::IsSrgbTextureFormat(oxrsys::video::MetalBgra8UnormSrgb));
}
