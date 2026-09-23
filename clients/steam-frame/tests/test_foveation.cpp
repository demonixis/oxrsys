// SPDX-License-Identifier: BSL-1.0
// Foveation reconstruction accuracy across every preset (Light/Medium/High).
//
// The stream path was only ever verified visually at Medium. This exercises the
// warp math the shader relies on, headless, at the Frame's real per-eye size:
//   - the client fills output[u] from encoded[decompressAxis(u)], and the server
//     stored encoded[e] = render[compressAxis(e)]; so compressAxis(decompressAxis(u))
//     must equal u for the reconstruction to be faithful. That composition is the
//     bisection inverse's real error budget, measured here in pixels.
//   - the aligned params come from the shared Foveation.h layout math, the same
//     call the client makes on connect, so we test what actually ships.
#include "test_util.h"
#include "foveation_warp.h"

#include <oxrsys/protocol/Foveation.h>

#include <cmath>
#include <cstdint>

using namespace oxr;
using fov_oracle::compressAxis;
using fov_oracle::decompressAxis;

namespace {

struct AxisResult {
    float maxRoundTripPx = 0.0f;  // |compress(decompress(u)) - u| * targetPx
    float maxInversePx = 0.0f;    // |decompress(compress(u)) - u| * targetPx
    float maxDeviation = 0.0f;    // |compress(u) - u|, "is the warp non-trivial"
    bool monotonic = true;
    float endLo = 0.0f, endHi = 1.0f;
};

AxisResult scanAxis(float centerSize, float centerShift, float edgeRatio, uint32_t targetPx)
{
    AxisResult r;
    const int N = 4096;
    float prev = -1.0f;
    for (int i = 0; i <= N; ++i) {
        float u = (float)i / (float)N;

        float rt = compressAxis(decompressAxis(u, centerSize, centerShift, edgeRatio),
                                centerSize, centerShift, edgeRatio);
        r.maxRoundTripPx = std::fmax(r.maxRoundTripPx, std::fabs(rt - u) * targetPx);

        float inv = decompressAxis(compressAxis(u, centerSize, centerShift, edgeRatio),
                                   centerSize, centerShift, edgeRatio);
        r.maxInversePx = std::fmax(r.maxInversePx, std::fabs(inv - u) * targetPx);

        float c = compressAxis(u, centerSize, centerShift, edgeRatio);
        r.maxDeviation = std::fmax(r.maxDeviation, std::fabs(c - u));
        if (c < prev - 1e-6f) r.monotonic = false;
        prev = c;

        float d = decompressAxis(u, centerSize, centerShift, edgeRatio);
        if (d < -1e-4f || d > 1.0f + 1e-4f) r.monotonic = false;  // stays in [0,1]
    }
    r.endLo = compressAxis(0.0f, centerSize, centerShift, edgeRatio);
    r.endHi = compressAxis(1.0f, centerSize, centerShift, edgeRatio);
    return r;
}

void checkPreset(const char* name, protocol::FoveationPreset preset, uint32_t eyePx)
{
    protocol::FoveationLayout layout =
        protocol::CalculateFoveationLayout(eyePx, eyePx, preset);

    // The layout the client would actually accept must validate.
    CHECK(protocol::IsFoveatedEncodingLayoutUsable(layout, eyePx, eyePx));

    const auto& p = layout.parameters;
    AxisResult x = scanAxis(p.centerSizeX, p.centerShiftX, p.edgeRatioX, eyePx);
    AxisResult y = scanAxis(p.centerSizeY, p.centerShiftY, p.edgeRatioY, eyePx);

    printf("  %-6s  X: reconstruct<=%.2fpx inverse<=%.2fpx warp=%.3f  |  "
           "Y: reconstruct<=%.2fpx inverse<=%.2fpx warp=%.3f  (encoded %ux%u)\n",
           name, x.maxRoundTripPx, x.maxInversePx, x.maxDeviation,
           y.maxRoundTripPx, y.maxInversePx, y.maxDeviation,
           layout.optimizedEyeWidth, layout.optimizedEyeHeight);

    // Sub-pixel reconstruction: the center must be recovered essentially exactly.
    CHECK(x.maxRoundTripPx < 1.0f);
    CHECK(y.maxRoundTripPx < 1.0f);
    // The bisection is a true inverse both ways to sub-pixel accuracy.
    CHECK(x.maxInversePx < 1.0f);
    CHECK(y.maxInversePx < 1.0f);
    // A foveated preset must actually warp (otherwise it saves no bandwidth).
    CHECK(x.maxDeviation > 0.01f);
    CHECK(y.maxDeviation > 0.01f);
    // Warp is monotonic and preserves the [0,1] endpoints.
    CHECK(x.monotonic);
    CHECK(y.monotonic);
    CHECK(std::fabs(x.endLo) < 1e-3f && std::fabs(x.endHi - 1.0f) < 1e-3f);
    CHECK(std::fabs(y.endLo) < 1e-3f && std::fabs(y.endHi - 1.0f) < 1e-3f);
    // Foveation shrinks the transmitted frame in both axes.
    CHECK(layout.optimizedEyeWidth < eyePx);
    CHECK(layout.optimizedEyeHeight < eyePx);
}

// Gaze-driven centre: the server shifts the foveal region to follow the eye, and the client has
// to reconstruct with the exact same parameters or the un-warp is wrong. Three things must hold:
// the encoded size must not change (or the encoder would need reconfiguring every frame), the
// quantized round trip must be bit-exact, and reconstruction must stay sub-pixel off-centre.
void checkGazeShift(protocol::FoveationPreset preset, uint32_t eyePx, float gazeX, float gazeY)
{
    protocol::FoveationLayout base = protocol::CalculateFoveationLayout(eyePx, eyePx, preset);
    const uint32_t baseW = base.optimizedEyeWidth;
    const uint32_t baseH = base.optimizedEyeHeight;

    protocol::FoveationLayout server = base;
    protocol::ApplyGazeCenterShift(server, gazeX, gazeY);

    // The client only ever sees the two quantized bytes from the packet header.
    protocol::FoveationLayout client = base;
    protocol::ApplyQuantizedCenterShift(client,
                                        protocol::QuantizeCenterShift(gazeX),
                                        protocol::QuantizeCenterShift(gazeY));

    // Encoded size is untouched: centre placement must never resize the stream.
    CHECK(server.optimizedEyeWidth == baseW);
    CHECK(server.optimizedEyeHeight == baseH);

    // Server and client agree exactly, not approximately.
    CHECK(server.parameters.centerShiftX == client.parameters.centerShiftX);
    CHECK(server.parameters.centerShiftY == client.parameters.centerShiftY);
    CHECK(server.parameters.centerSizeX == client.parameters.centerSizeX);
    CHECK(server.parameters.centerSizeY == client.parameters.centerSizeY);

    // Reconstruction stays sub-pixel with the centre moved off-axis.
    const auto& p = server.parameters;
    AxisResult x = scanAxis(p.centerSizeX, p.centerShiftX, p.edgeRatioX, eyePx);
    AxisResult y = scanAxis(p.centerSizeY, p.centerShiftY, p.edgeRatioY, eyePx);
    printf("  gaze(%+.2f,%+.2f) shift=(%+.4f,%+.4f) X:%.2fpx Y:%.2fpx encoded %ux%u\n",
           gazeX, gazeY, p.centerShiftX, p.centerShiftY,
           x.maxRoundTripPx, y.maxRoundTripPx, server.optimizedEyeWidth, server.optimizedEyeHeight);
    CHECK(x.maxRoundTripPx < 1.0f);
    CHECK(y.maxRoundTripPx < 1.0f);
    CHECK(x.monotonic);
    CHECK(y.monotonic);
    CHECK(std::fabs(x.endLo) < 1e-3f && std::fabs(x.endHi - 1.0f) < 1e-3f);
    CHECK(std::fabs(y.endLo) < 1e-3f && std::fabs(y.endHi - 1.0f) < 1e-3f);
}

} // namespace

void test_foveation()
{
    SECTION("foveation: warp reconstruction across presets (Frame per-eye 2160)");
    const uint32_t eyePx = 2160;  // Steam Frame per-eye
    checkPreset("Light", protocol::FoveationPreset::Light, eyePx);
    checkPreset("Medium", protocol::FoveationPreset::Medium, eyePx);
    checkPreset("High", protocol::FoveationPreset::High, eyePx);

    // Off preset: no warp, no shrink, layout not "usable" as foveated.
    protocol::FoveationLayout off =
        protocol::CalculateFoveationLayout(eyePx, eyePx, protocol::FoveationPreset::Off);
    CHECK(!protocol::IsFoveatedEncodingLayoutUsable(off, eyePx, eyePx));
    CHECK(off.optimizedEyeWidth == eyePx && off.optimizedEyeHeight == eyePx);

    SECTION("foveation: gaze-driven centre (server/client agreement, stable encoded size)");
    const float sweep[] = {-1.0f, -0.6f, -0.25f, 0.0f, 0.25f, 0.6f, 1.0f};
    for (float gx : sweep)
    {
        for (float gy : sweep)
        {
            checkGazeShift(protocol::FoveationPreset::Medium, eyePx, gx, gy);
        }
    }
    // Every preset must tolerate an extreme gaze without breaking reconstruction.
    checkGazeShift(protocol::FoveationPreset::Light, eyePx, 1.0f, -1.0f);
    checkGazeShift(protocol::FoveationPreset::High, eyePx, -1.0f, 1.0f);

    // Off stays off: a gaze shift on a disabled preset must not invent a warp.
    protocol::FoveationLayout offShifted = off;
    protocol::ApplyGazeCenterShift(offShifted, 1.0f, 1.0f);
    CHECK(offShifted.parameters.centerShiftX == 0.0f);
    CHECK(offShifted.parameters.centerShiftY == 0.0f);

    // Quantization is symmetric and lossless at the endpoints we rely on.
    CHECK(protocol::QuantizeCenterShift(0.0f) == 0);
    CHECK(protocol::QuantizeCenterShift(1.0f) == 127);
    CHECK(protocol::QuantizeCenterShift(-1.0f) == -127);
    CHECK(protocol::QuantizeCenterShift(2.0f) == 127);   // clamped
    CHECK(protocol::DequantizeCenterShift(127) == 1.0f);
    CHECK(protocol::DequantizeCenterShift(-127) == -1.0f);
}
