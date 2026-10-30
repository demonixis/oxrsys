// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cmath>

namespace oxr::quest_foveation
{

// CPU copy of decompressAxisAndSlope in XrApp.cpp's BLIT_FRAGMENT_SHADER_OES, kept for the host
// tests. GLSL cannot include C++, so change both together.
//
// Closed-form inverse of the server's AADT compress_axis warp (runtime/src/VideoEncoder.mm), the
// same solution as the visionOS client's decompressAxis: linear in the center, the stable small
// root of a quadratic on each edge. invSlope is d(eyeUv)/d(encodedUv) at the result, taken from
// the edge polynomial's derivative.
inline float DecompressAxisAndSlope(float targetUv, float centerSize, float centerShift,
                                    float edgeRatio, float& invSlope)
{
    float c0 = (1.0f - centerSize) * 0.5f;
    float c1 = (edgeRatio - 1.0f) * c0 * (centerShift + 1.0f) / edgeRatio;
    float c2 = (edgeRatio - 1.0f) * centerSize + 1.0f;
    float loBound = c0 * (centerShift + 1.0f) / c2;
    float hiBound = c0 * (centerShift - 1.0f) / c2 + 1.0f;
    float tLo = loBound * c2 / edgeRatio + c1;
    float tHi = hiBound * c2 / edgeRatio + c1;

    float eyeUv;
    float slope;
    if (targetUv < tLo && loBound > 0.0f)
    {
        // Left edge: t = a*u^2 + b*u with a < 0, b > 0; small root through (0, 0).
        float a = (c2 / loBound) * (1.0f / edgeRatio - 1.0f);
        float b = c1 / loBound + c2;
        eyeUv = 2.0f * targetUv / (b + std::sqrt(std::fmax(b * b + 4.0f * a * targetUv, 0.0f)));
        slope = 2.0f * a * eyeUv + b;
    }
    else if (targetUv > tHi && hiBound < 1.0f)
    {
        // Right edge: with v = 1-u, t-1 = a*v^2 + b*v with a > 0, b < 0; small root through
        // (t=1, v=0).
        float w = 1.0f - hiBound;
        float a = (c2 / w) * (1.0f - 1.0f / edgeRatio);
        float b = (c2 / edgeRatio + c1 - 1.0f) / w - c2;
        float d = std::fmax(b * b - 4.0f * a * (1.0f - targetUv), 0.0f);
        float v = 2.0f * (1.0f - targetUv) / (-b + std::sqrt(d));
        eyeUv = 1.0f - v;
        slope = -(2.0f * a * v + b);
    }
    else
    {
        eyeUv = (targetUv - c1) * edgeRatio / c2;
        slope = c2 / edgeRatio;
    }
    invSlope = std::fabs(slope) > 0.0001f ? 1.0f / slope : 1.0f;
    return eyeUv;
}

} // namespace oxr::quest_foveation
