// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "CompositionLayerAlpha.h"

using namespace oxrsys::runtime;

TEST_CASE("Source-alpha projection layers mark passthrough streaming frames alpha", "[runtime][layers]")
{
    constexpr XrCompositionLayerFlags sourceAlpha =
        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;

    REQUIRE_FALSE(IsSourceAlphaProjectionLayerForStreaming(0, true));
    REQUIRE_FALSE(IsSourceAlphaProjectionLayerForStreaming(sourceAlpha, false));
    REQUIRE(IsSourceAlphaProjectionLayerForStreaming(sourceAlpha, true));

    REQUIRE_FALSE(IsAlphaFrameForStreaming(XR_ENVIRONMENT_BLEND_MODE_OPAQUE, false));
    REQUIRE(IsAlphaFrameForStreaming(XR_ENVIRONMENT_BLEND_MODE_OPAQUE, true));
    REQUIRE(IsAlphaFrameForStreaming(XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND, false));
}
