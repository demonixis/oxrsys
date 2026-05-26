// SPDX-License-Identifier: MPL-2.0

//
//  Shaders.metal
//  OXRSys visionOS
//
//  Created by Yannick Comte on 21/03/2026.
//

#include <metal_stdlib>

using namespace metal;

struct StereoVertexOut {
    float4 position [[position]];
    float2 texCoord;
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

fragment float4 stereoImmersiveFragment(
    StereoVertexOut in [[stage_in]],
    texture2d<float> lumaTexture [[texture(0)]],
    texture2d<float> chromaTexture [[texture(1)]]
) {
    constexpr sampler textureSampler(address::clamp_to_edge,
                                     mag_filter::linear,
                                     min_filter::linear);

    if (!lumaTexture.get_width() || !chromaTexture.get_width()) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float eyeOffset = in.eyeIndex * 0.5;
    float2 stereoUV = float2(in.texCoord.x * 0.5 + eyeOffset, in.texCoord.y);

    float y = lumaTexture.sample(textureSampler, stereoUV).r;
    float2 cbcr = chromaTexture.sample(textureSampler, stereoUV).rg - float2(0.5, 0.5);

    float r = y + 1.4020 * cbcr.y;
    float g = y - 0.3441 * cbcr.x - 0.7141 * cbcr.y;
    float b = y + 1.7720 * cbcr.x;
    return float4(r, g, b, 1.0);
}
