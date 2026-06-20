// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>

namespace oxrsys::runtime
{

inline bool IsSourceAlphaProjectionLayerForStreaming(XrCompositionLayerFlags layerFlags,
                                                     bool passthroughEnabled)
{
    return passthroughEnabled &&
           (layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
}

inline bool IsAlphaFrameForStreaming(XrEnvironmentBlendMode environmentBlendMode,
                                     bool hasSourceAlphaProjectionLayer)
{
    return environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND ||
           hasSourceAlphaProjectionLayer;
}

} // namespace oxrsys::runtime
