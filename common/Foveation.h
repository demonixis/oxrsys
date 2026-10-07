// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Protocol.h"

namespace oxr
{
namespace protocol
{

// Foveated encoding variables are adapted from ALVR's axis-aligned foveated
// encoding math (MIT licensed). Presets and wire plumbing remain OXRSys code.
struct FoveationPresetParameters
{
    float centerSizeX = 1.0f;
    float centerSizeY = 1.0f;
    float centerShiftX = 0.0f;
    float centerShiftY = 0.0f;
    float edgeRatioX = 1.0f;
    float edgeRatioY = 1.0f;
};

struct FoveationLayout
{
    FoveationPreset preset = FoveationPreset::Off;
    uint32_t targetEyeWidth = 0;
    uint32_t targetEyeHeight = 0;
    uint32_t optimizedEyeWidth = 0;
    uint32_t optimizedEyeHeight = 0;
    float eyeWidthRatio = 1.0f;
    float eyeHeightRatio = 1.0f;
    FoveationPresetParameters parameters = {};
};

inline FoveationPresetParameters ParametersForPreset(FoveationPreset preset)
{
    switch (preset)
    {
    case FoveationPreset::Light:
        return {0.60f, 0.55f, 0.0f, 0.0f, 2.0f, 3.0f};
    case FoveationPreset::Medium:
        return {0.45f, 0.40f, 0.0f, 0.0f, 4.0f, 5.0f};
    case FoveationPreset::High:
        return {0.35f, 0.32f, 0.0f, 0.0f, 6.0f, 7.0f};
    case FoveationPreset::Off:
    default:
        return {};
    }
}

inline uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return alignment == 0 ? value : ((value + alignment - 1u) / alignment) * alignment;
}

inline FoveationLayout CalculateFoveationLayout(uint32_t targetEyeWidth,
                                                uint32_t targetEyeHeight,
                                                FoveationPreset preset,
                                                uint32_t alignment = 32)
{
    FoveationLayout layout = {};
    layout.preset = preset;
    layout.targetEyeWidth = targetEyeWidth;
    layout.targetEyeHeight = targetEyeHeight;
    layout.optimizedEyeWidth = targetEyeWidth;
    layout.optimizedEyeHeight = targetEyeHeight;

    FoveationPresetParameters params = ParametersForPreset(preset);
    if (preset == FoveationPreset::Off ||
        targetEyeWidth == 0 ||
        targetEyeHeight == 0 ||
        params.edgeRatioX <= 1.0f ||
        params.edgeRatioY <= 1.0f)
    {
        layout.parameters = params;
        return layout;
    }

    const float targetW = static_cast<float>(targetEyeWidth);
    const float targetH = static_cast<float>(targetEyeHeight);
    const float edgeSizeX = targetW - params.centerSizeX * targetW;
    const float edgeSizeY = targetH - params.centerSizeY * targetH;

    const float centerSizeXAligned =
        1.0f - std::ceil(edgeSizeX / (params.edgeRatioX * 2.0f)) *
                   (params.edgeRatioX * 2.0f) / targetW;
    const float centerSizeYAligned =
        1.0f - std::ceil(edgeSizeY / (params.edgeRatioY * 2.0f)) *
                   (params.edgeRatioY * 2.0f) / targetH;

    const float edgeSizeXAligned = targetW - centerSizeXAligned * targetW;
    const float edgeSizeYAligned = targetH - centerSizeYAligned * targetH;

    float centerShiftXAligned = 0.0f;
    float centerShiftYAligned = 0.0f;
    if (edgeSizeXAligned > 0.0f)
    {
        centerShiftXAligned =
            std::ceil(params.centerShiftX * edgeSizeXAligned / (params.edgeRatioX * 2.0f)) *
            (params.edgeRatioX * 2.0f) / edgeSizeXAligned;
    }
    if (edgeSizeYAligned > 0.0f)
    {
        centerShiftYAligned =
            std::ceil(params.centerShiftY * edgeSizeYAligned / (params.edgeRatioY * 2.0f)) *
            (params.edgeRatioY * 2.0f) / edgeSizeYAligned;
    }

    params.centerSizeX = std::clamp(centerSizeXAligned, 0.0f, 1.0f);
    params.centerSizeY = std::clamp(centerSizeYAligned, 0.0f, 1.0f);
    params.centerShiftX = centerShiftXAligned;
    params.centerShiftY = centerShiftYAligned;

    const float foveationScaleX = params.centerSizeX + (1.0f - params.centerSizeX) / params.edgeRatioX;
    const float foveationScaleY = params.centerSizeY + (1.0f - params.centerSizeY) / params.edgeRatioY;
    const float optimizedW = foveationScaleX * targetW;
    const float optimizedH = foveationScaleY * targetH;

    layout.optimizedEyeWidth = AlignUp(static_cast<uint32_t>(std::ceil(optimizedW)),
                                       std::max(alignment, 1u));
    layout.optimizedEyeHeight = AlignUp(static_cast<uint32_t>(std::ceil(optimizedH)),
                                        std::max(alignment, 1u));
    layout.eyeWidthRatio = optimizedW / static_cast<float>(std::max(layout.optimizedEyeWidth, 1u));
    layout.eyeHeightRatio = optimizedH / static_cast<float>(std::max(layout.optimizedEyeHeight, 1u));
    layout.parameters = params;
    return layout;
}

inline bool IsFoveatedEncodingLayoutUsable(const FoveationLayout& layout,
                                           uint32_t sourceEyeWidth,
                                           uint32_t sourceEyeHeight)
{
    return layout.preset != FoveationPreset::Off &&
           layout.targetEyeWidth == sourceEyeWidth &&
           layout.targetEyeHeight == sourceEyeHeight &&
           layout.optimizedEyeWidth > 0 &&
           layout.optimizedEyeHeight > 0 &&
           layout.optimizedEyeWidth <= layout.targetEyeWidth &&
           layout.optimizedEyeHeight <= layout.targetEyeHeight &&
           std::isfinite(layout.eyeWidthRatio) &&
           std::isfinite(layout.eyeHeightRatio) &&
           layout.eyeWidthRatio > 0.0f &&
           layout.eyeWidthRatio <= 1.0f &&
           layout.eyeHeightRatio > 0.0f &&
           layout.eyeHeightRatio <= 1.0f &&
           std::isfinite(layout.parameters.centerSizeX) &&
           std::isfinite(layout.parameters.centerSizeY) &&
           layout.parameters.centerSizeX >= 0.0f &&
           layout.parameters.centerSizeX <= 1.0f &&
           layout.parameters.centerSizeY >= 0.0f &&
           layout.parameters.centerSizeY <= 1.0f &&
           std::isfinite(layout.parameters.centerShiftX) &&
           std::isfinite(layout.parameters.centerShiftY) &&
           std::isfinite(layout.parameters.edgeRatioX) &&
           std::isfinite(layout.parameters.edgeRatioY) &&
           layout.parameters.edgeRatioX > 1.0f &&
           layout.parameters.edgeRatioY > 1.0f;
}

} // namespace protocol
} // namespace oxr
