// SPDX-License-Identifier: BSL-1.0
#pragma once

// CPU mirror of the axis-aligned foveation warp in shaders/sample.frag, used as
// a test oracle. GLSL cannot be shared with C++, so these two copies must stay
// in sync by hand: if compressAxis/decompressAxis change here, change the shader
// and vice versa. Kept float (not double) so the numeric behavior matches the
// shader's 10-iteration bisection inverse.

namespace fov_oracle {

// Forward map: encoded/optimized UV -> full-resolution eye UV. This is the map
// the oxrsys server applies when it foveates a frame; the client inverts it.
inline float compressAxis(float eyeUv, float centerSize, float centerShift, float edgeRatio)
{
    float c0 = (1.0f - centerSize) * 0.5f;
    float c1 = (edgeRatio - 1.0f) * c0 * (centerShift + 1.0f) / edgeRatio;
    float c2 = (edgeRatio - 1.0f) * centerSize + 1.0f;
    float loBound = c0 * (centerShift + 1.0f) / c2;
    float hiBound = c0 * (centerShift - 1.0f) / c2 + 1.0f;
    float center = eyeUv * c2 / edgeRatio + c1;
    float d2 = eyeUv * c2;
    float d3 = (eyeUv - 1.0f) * c2 + 1.0f;
    float g1 = loBound > 0.0f ? eyeUv / loBound : 1.0f;
    float g2 = (1.0f - hiBound) > 0.0f ? (1.0f - eyeUv) / (1.0f - hiBound) : 1.0f;
    float leftEdge = g1 * center + (1.0f - g1) * d2;
    float rightEdge = g2 * center + (1.0f - g2) * d3;
    if (eyeUv < loBound) return leftEdge;
    if (eyeUv > hiBound) return rightEdge;
    return center;
}

// Binary-search inverse (matches the shader: 16 iterations).
inline float decompressAxis(float targetUv, float centerSize, float centerShift, float edgeRatio)
{
    float lo = 0.0f, hi = 1.0f, mid = 0.5f;
    for (int i = 0; i < 16; ++i) {
        mid = (lo + hi) * 0.5f;
        if (compressAxis(mid, centerSize, centerShift, edgeRatio) < targetUv) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5f;
}

} // namespace fov_oracle
