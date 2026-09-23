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

// Snap a requested center shift onto the same grid the edge compression uses. Server and client
// must call this with identical inputs or the client's un-warp will not match the server's warp.
inline float AlignCenterShift(float requestedShift,
                              float centerSizeAligned,
                              float targetSize,
                              float edgeRatio)
{
    const float edgeSizeAligned = targetSize - centerSizeAligned * targetSize;
    if (edgeSizeAligned <= 0.0f)
    {
        return 0.0f;
    }
    // Round to nearest: std::ceil would bias every off-grid value toward +1, drifting the foveal
    // region right/down by up to one grid cell. Harmless while the shift was always zero; real
    // once gaze drives it. edgeSizeAligned is a multiple of 2 * edgeRatio up to float error, so
    // +-1.0 land within an ulp of exact and the clamp holds the shader's |shift| <= 1 invariant.
    const float aligned = std::round(requestedShift * edgeSizeAligned / (edgeRatio * 2.0f)) *
                          (edgeRatio * 2.0f) / edgeSizeAligned;
    return std::clamp(aligned, -1.0f, 1.0f);
}

// Wire representation of a gaze-driven center shift. Quantized so the value the server warps with
// is exactly the value the client un-warps with: quantize first, then use the dequantized result
// on both sides. Two int8s ride in VideoPacketHeader's spare bytes, so this costs no bandwidth.
inline int8_t QuantizeCenterShift(float shift)
{
    // NaN falls through std::clamp unchanged and std::lround on it is unspecified; the caller
    // guards its inputs, but a poisoned value must never reach the wire.
    if (std::isnan(shift))
    {
        return 0;
    }
    const float clamped = std::clamp(shift, -1.0f, 1.0f);
    return static_cast<int8_t>(std::lround(clamped * 127.0f));
}

inline float DequantizeCenterShift(int8_t quantized)
{
    return static_cast<float>(quantized) / 127.0f;
}

// The one decode both ends must share: the server's warp and the client's un-warp each turn the
// transmitted byte into a shift through this exact call, so the reconstruction is bit-identical.
inline float DecodeCenterShift(int8_t quantized,
                               float centerSizeAligned,
                               float targetSize,
                               float edgeRatio)
{
    return AlignCenterShift(
        DequantizeCenterShift(quantized), centerSizeAligned, targetSize, edgeRatio);
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

    const float centerShiftXAligned =
        AlignCenterShift(params.centerShiftX, centerSizeXAligned, targetW, params.edgeRatioX);
    const float centerShiftYAligned =
        AlignCenterShift(params.centerShiftY, centerSizeYAligned, targetH, params.edgeRatioY);

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

// The receiving half of the above: reconstruct the exact layout the server warped with, from the
// two quantized bytes carried in VideoPacketHeader.
inline void ApplyQuantizedCenterShift(FoveationLayout& layout, int8_t quantX, int8_t quantY)
{
    if (layout.preset == FoveationPreset::Off || layout.targetEyeWidth == 0 ||
        layout.targetEyeHeight == 0)
    {
        return;
    }

    const float targetW = static_cast<float>(layout.targetEyeWidth);
    const float targetH = static_cast<float>(layout.targetEyeHeight);
    layout.parameters.centerShiftX = DecodeCenterShift(quantX,
                                                       layout.parameters.centerSizeX,
                                                       targetW,
                                                       layout.parameters.edgeRatioX);
    layout.parameters.centerShiftY = DecodeCenterShift(quantY,
                                                       layout.parameters.centerSizeY,
                                                       targetH,
                                                       layout.parameters.edgeRatioY);
}

// Move the foveal region to follow gaze. Safe to call per frame: the optimized (encoded) size
// depends only on centerSize and edgeRatio, so shifting the center never resizes the stream and
// never needs an encoder reconfigure.
//
// shiftX/shiftY are normalized gaze offsets from the eye's view centre in [-1, 1]. They are
// quantized to the wire representation before use so that a client decoding the quantized value
// from the packet header reproduces this layout bit-for-bit.
inline void ApplyGazeCenterShift(FoveationLayout& layout, float shiftX, float shiftY)
{
    if (layout.preset == FoveationPreset::Off || layout.targetEyeWidth == 0 ||
        layout.targetEyeHeight == 0)
    {
        return;
    }

    ApplyQuantizedCenterShift(layout,
                              QuantizeCenterShift(shiftX),
                              QuantizeCenterShift(shiftY));
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
