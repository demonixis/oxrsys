// SPDX-License-Identifier: MPL-2.0

//
//  Shaders.metal
//  OXRSys visionOS
//
//  Created by Yannick Comte on 21/03/2026.
//

#include <metal_stdlib>

using namespace metal;

// Per-eye asynchronous timewarp. The streamed frame was rendered for the head pose the server
// reports with it; every vsync we reproject it into the live head pose by rotating each output
// ray into the render-eye frame and resampling. This keeps the world locked to the head as it
// rotates (the same idea Quest Link / Virtual Desktop / ALVR use), with no tuning constants —
// only the eye's real FOV tangents and the rotation between the two head poses.
struct ReprojData {
    float3x3 rot;       // maps a current-eye ray direction into render-eye space (R_render^-1 * R_current)
    float4 tangents;    // (left, right, up, down) positive tangent magnitudes for this eye
    float3 translation; // (current - render) eye position in render-eye space, divided by the plane distance
};

struct VideoColorParams {
    float4 range; // luma offset, luma scale, chroma center, chroma scale
};

// Foveated-encoding (AADT) parameters. `enabled == 0` is an exact passthrough, so this is inert
// unless the server is actually sending a foveated stream.
struct FoveationParams {
    uint enabled;
    uint _pad;
    float2 centerSize;
    float2 centerShift;
    float2 edgeRatio;
    float2 eyeSizeRatio; // foveated content fraction of the encoded eye region per axis
};

// Closed-form inverse of the server's AADT compress_axis warp (runtime/src/VideoEncoder.mm):
// maps a displayed eye-UV back to the encoded texel it came from. The forward warp is
// piecewise — quadratic on [0, loBound), linear on [loBound, hiBound], quadratic on
// (hiBound, 1] — and each piece is monotonic, so the inverse is the linear solution in the
// center and the stable small root of a quadratic at the edges. Verified against the server
// warp to fp32 precision (max round-trip error ~2e-7 across all foveation presets, vs ~5e-4
// for the 10-step bisection this replaces).
static float decompressAxis(float t, float centerSize, float centerShift, float edgeRatio) {
    float c0 = (1.0 - centerSize) * 0.5;
    float c1 = (edgeRatio - 1.0) * c0 * (centerShift + 1.0) / edgeRatio;
    float c2 = (edgeRatio - 1.0) * centerSize + 1.0;
    float loBound = c0 * (centerShift + 1.0) / c2;
    float hiBound = c0 * (centerShift - 1.0) / c2 + 1.0;
    float tLo = loBound * c2 / edgeRatio + c1;
    float tHi = hiBound * c2 / edgeRatio + c1;

    if (t < tLo && loBound > 0.0) {
        // Left edge: t = a*u^2 + b*u with a < 0, b > 0; small root through (0, 0).
        float a = (c2 / loBound) * (1.0 / edgeRatio - 1.0);
        float b = c1 / loBound + c2;
        return 2.0 * t / (b + sqrt(max(b * b + 4.0 * a * t, 0.0)));
    }
    if (t > tHi && hiBound < 1.0) {
        // Right edge: with v = 1-u, t-1 = a*v^2 + b*v with a > 0, b < 0; small root
        // through (t=1, v=0) via the numerically stable conjugate form.
        float w = 1.0 - hiBound;
        float a = (c2 / w) * (1.0 - 1.0 / edgeRatio);
        float b = (c2 / edgeRatio + c1 - 1.0) / w - c2;
        float d = max(b * b - 4.0 * a * (1.0 - t), 0.0);
        return 1.0 - 2.0 * (1.0 - t) / (-b + sqrt(d));
    }
    return (t - c1) * edgeRatio / c2;
}

struct StereoVertexOut {
    float4 position [[position]];
    float2 texCoord; // output-view screen position in [0,1] for this eye
    float eyeIndex;
};

vertex StereoVertexOut stereoImmersiveVertex(uint vertexID [[vertex_id]],
                                             ushort amp_id [[amplification_id]]) {
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    float2 texCoords[3] = {
        float2(0.0, 1.0),
        float2(2.0, 1.0),
        float2(0.0, -1.0)
    };

    StereoVertexOut out;
    out.position = float4(positions[vertexID], 0.0, 1.0);
    out.texCoord = texCoords[vertexID];
    out.eyeIndex = float(amp_id);
    return out;
}

// Per-frame post-processing parameters (source-space luma sharpening).
struct PostFXParams {
    float sharpen; // 0 = off; contrast-adaptive sharpen strength
    float _pad0;
    float _pad1;
    float _pad2;
};

// Maps one displayed eye-UV to its side-by-side video UV: rotational reprojection → foveation
// unwarp → stereo split. Computed once per fragment; the sharpening taps reuse the result and
// step in video texels, so they never re-run this mapping.
static float2 displayToStereoUV(float2 outCoord,
                                float eyeIndex,
                                ReprojData rd,
                                constant FoveationParams &fov) {
    float left = rd.tangents.x;
    float right = rd.tangents.y;
    float up = rd.tangents.z;
    float down = rd.tangents.w;

    // Ray for this output fragment in the current eye's frustum, at the z = -1 plane.
    // outCoord.y = 0 is the top of the view (+up), outCoord.y = 1 the bottom (-down).
    float x = mix(-left, right, outCoord.x);
    float y = mix(up, -down, outCoord.y);
    float3 dirCurrent = float3(x, y, -1.0);

    // Map into the pose the server rendered this frame for — rotation exactly, translation as
    // planar parallax: a point assumed at plane distance d along the current ray sits at
    // rot·dir·d + Δ in the render eye, so adding Δ/d to the rotated ray reprojects both the
    // render→now rotation AND translation (identity rot + zero Δ → eyeUV == outCoord).
    float3 dirRender = rd.rot * dirCurrent + rd.translation;
    float2 eyeUV = outCoord;
    if (dirRender.z < 0.0) {
        float zf = -dirRender.z;
        eyeUV = float2((dirRender.x / zf + left) / (left + right),
                       (up - dirRender.y / zf) / (up + down));
    }

    // Undo the server's foveated-encoding warp (passthrough when fov.enabled == 0): map this
    // displayed eye-UV back to the encoded texel it came from, then scale by the foveated
    // content's fraction of the encoded eye region.
    float2 sourceUV = eyeUV;
    if (fov.enabled != 0) {
        float2 t = clamp(eyeUV, 0.0, 1.0);
        sourceUV.x = decompressAxis(t.x, fov.centerSize.x, fov.centerShift.x, fov.edgeRatio.x)
            * fov.eyeSizeRatio.x;
        sourceUV.y = decompressAxis(t.y, fov.centerSize.y, fov.centerShift.y, fov.edgeRatio.y)
            * fov.eyeSizeRatio.y;
        sourceUV = clamp(sourceUV, 0.0, 1.0);
    }

    float eyeOffset = eyeIndex * 0.5;
    return float2(sourceUV.x * 0.5 + eyeOffset, sourceUV.y);
}

fragment float4 stereoImmersiveFragment(
    StereoVertexOut in [[stage_in]],
    texture2d<float> lumaTexture [[texture(0)]],
    texture2d<float> chromaTexture [[texture(1)]],
    constant ReprojData *reproj [[buffer(0)]],
    constant VideoColorParams &colorParams [[buffer(1)]],
    constant FoveationParams &fov [[buffer(2)]],
    constant PostFXParams &postfx [[buffer(3)]]
) {
    constexpr sampler textureSampler(address::clamp_to_edge,
                                     mag_filter::linear,
                                     min_filter::linear);

    if (!lumaTexture.get_width() || !chromaTexture.get_width()) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    ReprojData rd = reproj[uint(in.eyeIndex)];
    float2 stereoUV = displayToStereoUV(in.texCoord, in.eyeIndex, rd, fov);
    float yLuma = lumaTexture.sample(textureSampler, stereoUV).r;

    // Contrast-adaptive sharpening on luma only, in source (video) space: the 4 neighbour taps
    // step one video texel from the already-computed mapping instead of re-running the full
    // reprojection/unwarp per tap, and luma carries virtually all perceived sharpness (chroma
    // sharpening mostly adds ringing). Unsharp against the neighbour average, clamped to the
    // local min/max so detail crisps up without overshoot. Raw code values are fine here: the
    // later range expansion is affine, and this operation commutes with affine maps. Off at 0.
    if (postfx.sharpen > 0.0) {
        float2 d = 1.0 / float2(lumaTexture.get_width(), lumaTexture.get_height());
        // Keep horizontal taps inside this eye's half of the side-by-side frame so sharpening
        // never bleeds across the stereo seam.
        float xMin = in.eyeIndex * 0.5 + 0.5 * d.x;
        float xMax = in.eyeIndex * 0.5 + 0.5 - 0.5 * d.x;
        float l = lumaTexture.sample(textureSampler, float2(clamp(stereoUV.x - d.x, xMin, xMax), stereoUV.y)).r;
        float r = lumaTexture.sample(textureSampler, float2(clamp(stereoUV.x + d.x, xMin, xMax), stereoUV.y)).r;
        float u = lumaTexture.sample(textureSampler, float2(stereoUV.x, stereoUV.y - d.y)).r;
        float dn = lumaTexture.sample(textureSampler, float2(stereoUV.x, stereoUV.y + d.y)).r;
        float mn = min(yLuma, min(min(l, r), min(u, dn)));
        float mx = max(yLuma, max(max(l, r), max(u, dn)));
        float blurred = (l + r + u + dn) * 0.25;
        yLuma = clamp(yLuma + (yLuma - blurred) * postfx.sharpen, mn, mx);
    }

    float2 cbcr = chromaTexture.sample(textureSampler, stereoUV).rg;

    // The streaming contract is limited/video-range BT.709 SDR. Expand luma and chroma using
    // bit-depth-specific normalized code values supplied by the renderer, then convert to RGB.
    float luma = (yLuma - colorParams.range.x) * colorParams.range.y;
    float cb = (cbcr.x - colorParams.range.z) * colorParams.range.w;
    float cr = (cbcr.y - colorParams.range.z) * colorParams.range.w;
    float3 rgb = float3(luma + 1.5748 * cr,
                        luma - 0.1873 * cb - 0.4681 * cr,
                        luma + 1.8556 * cb);
    return float4(clamp(rgb, 0.0, 1.0), 1.0);
}
