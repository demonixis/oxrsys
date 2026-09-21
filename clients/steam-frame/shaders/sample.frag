#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D uVideo;

layout(push_constant) uniform Push {
    vec2 sourceMin;      // this eye's region in the (SBS) source texture
    vec2 sourceMax;
    vec2 centerSize;     // foveation params (aligned, from the server)
    vec2 centerShift;
    vec2 edgeRatio;
    vec2 eyeSizeRatio;   // optimized/aligned padding compensation
    int  foveated;       // 0 = plain SBS sampling
} pc;

// Axis-aligned foveated-encoding forward map (matches oxrsys / ALVR AADT).
float compressAxis(float eyeUv, float centerSize, float centerShift, float edgeRatio) {
    float c0 = (1.0 - centerSize) * 0.5;
    float c1 = (edgeRatio - 1.0) * c0 * (centerShift + 1.0) / edgeRatio;
    float c2 = (edgeRatio - 1.0) * centerSize + 1.0;
    float loBound = c0 * (centerShift + 1.0) / c2;
    float hiBound = c0 * (centerShift - 1.0) / c2 + 1.0;
    float center = eyeUv * c2 / edgeRatio + c1;
    float d2 = eyeUv * c2;
    float d3 = (eyeUv - 1.0) * c2 + 1.0;
    float g1 = loBound > 0.0 ? eyeUv / loBound : 1.0;
    float g2 = (1.0 - hiBound) > 0.0 ? (1.0 - eyeUv) / (1.0 - hiBound) : 1.0;
    float leftEdge = g1 * center + (1.0 - g1) * d2;
    float rightEdge = g2 * center + (1.0 - g2) * d3;
    if (eyeUv < loBound) return leftEdge;
    if (eyeUv > hiBound) return rightEdge;
    return center;
}

// Binary-search inverse of compressAxis (10 iterations; the warp is smooth).
float decompressAxis(float targetUv, float centerSize, float centerShift, float edgeRatio) {
    float lo = 0.0, hi = 1.0, mid = 0.5;
    for (int i = 0; i < 10; ++i) {
        mid = (lo + hi) * 0.5;
        if (compressAxis(mid, centerSize, centerShift, edgeRatio) < targetUv) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5;
}

void main() {
    vec2 eyeUv = clamp(vUV, vec2(0.0), vec2(1.0));
    vec2 corrected = eyeUv;
    if (pc.foveated != 0) {
        corrected.x = decompressAxis(eyeUv.x, pc.centerSize.x, pc.centerShift.x, pc.edgeRatio.x) * pc.eyeSizeRatio.x;
        corrected.y = decompressAxis(eyeUv.y, pc.centerSize.y, pc.centerShift.y, pc.edgeRatio.y) * pc.eyeSizeRatio.y;
    }
    corrected = clamp(corrected, vec2(0.0), vec2(1.0));
    vec2 sourceUv = mix(pc.sourceMin, pc.sourceMax, corrected);
    outColor = vec4(texture(uVideo, sourceUv).rgb, 1.0);
}
