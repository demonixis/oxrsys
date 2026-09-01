// SPDX-License-Identifier: MPL-2.0

#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

struct FragmentUniforms {
    float ipdOffset;
    uint displayMode;
};

// Full-screen quad: 2 triangles from vertex_id (0..5)
vertex VertexOut stereoVertex(uint vid [[vertex_id]]) {
    // Triangle strip positions for a full-screen quad
    const float2 positions[] = {
        float2(-1, -1), float2( 1, -1), float2(-1,  1),
        float2(-1,  1), float2( 1, -1), float2( 1,  1)
    };
    const float2 texCoords[] = {
        float2(0, 1), float2(1, 1), float2(0, 0),
        float2(0, 0), float2(1, 1), float2(1, 0)
    };

    VertexOut out;
    out.position = float4(positions[vid], 0, 1);
    out.texCoord = texCoords[vid];
    return out;
}

fragment float4 stereoFragment(VertexOut in [[stage_in]],
                                texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear,
                        address::clamp_to_edge);
    return tex.sample(s, in.texCoord);
}

// YCbCr → RGB conversion for BiPlanar (NV12) video frames.
// ipdOffset (buffer 0): UV-space horizontal shift applied symmetrically to each eye.
// Positive value shifts images apart (wider separation); negative brings them together.
fragment float4 stereoFragmentYCbCr(VertexOut in [[stage_in]],
                                     texture2d<float> texY [[texture(0)]],
                                     texture2d<float> texCbCr [[texture(1)]],
                                     constant FragmentUniforms& uniforms [[buffer(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear,
                        address::clamp_to_edge);

    float2 uv = in.texCoord;
    if (uniforms.displayMode == 1) {
        uv.x *= 0.5;
    } else {
        // Left eye (u < 0.5) shifts left; right eye (u ≥ 0.5) shifts right.
        uv.x += (uv.x < 0.5) ? -uniforms.ipdOffset : uniforms.ipdOffset;
    }

    float y = texY.sample(s, uv).r;
    float2 cbcr = texCbCr.sample(s, uv).rg;

    // BT.709 YCbCr → RGB
    float cb = cbcr.x - 0.5;
    float cr = cbcr.y - 0.5;

    float r = y + 1.5748 * cr;
    float g = y - 0.1873 * cb - 0.4681 * cr;
    float b = y + 1.8556 * cb;

    return float4(r, g, b, 1.0);
}
