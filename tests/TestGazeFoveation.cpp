// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "GazeFoveation.h"

#include <oxrsys/protocol/Foveation.h>
#include <oxrsys/protocol/Protocol.h>

#include <cmath>
#include <limits>

using namespace oxr::protocol;
using oxrsys::gaze_foveation::ComputeGazeCenterOffset;
using oxrsys::gaze_foveation::GazeCenterFilter;
using oxrsys::gaze_foveation::GazeCenterOffset;

namespace
{

// Line-for-line port of compress_axis from the foveation Metal kernel in VideoEncoder.mm. The
// GPU cannot run in these tests, so the geometric assertions below rely on this replica staying
// identical to the shader source.
float CompressAxis(float eyeUv, float centerSize, float centerShift, float edgeRatio)
{
    const float c0 = (1.0f - centerSize) * 0.5f;
    const float c1 = (edgeRatio - 1.0f) * c0 * (centerShift + 1.0f) / edgeRatio;
    const float c2 = (edgeRatio - 1.0f) * centerSize + 1.0f;
    const float loBound = c0 * (centerShift + 1.0f) / c2;
    const float hiBound = c0 * (centerShift - 1.0f) / c2 + 1.0f;

    const float center = eyeUv * c2 / edgeRatio + c1;
    const float d2 = eyeUv * c2;
    const float d3 = (eyeUv - 1.0f) * c2 + 1.0f;
    const float g1 = loBound > 0.0f ? eyeUv / loBound : 1.0f;
    const float g2 = (1.0f - hiBound) > 0.0f ? (1.0f - eyeUv) / (1.0f - hiBound) : 1.0f;
    const float leftEdge = g1 * center + (1.0f - g1) * d2;
    const float rightEdge = g2 * center + (1.0f - g2) * d3;

    if (eyeUv < loBound)
    {
        return leftEdge;
    }
    if (eyeUv > hiBound)
    {
        return rightEdge;
    }
    return center;
}

// Centre of the sharp region in source UV: the midpoint of the kernel's identity interval
// [loBound, hiBound] mapped through its centre branch.
float FovealCenterSourceUv(float centerSize, float centerShift, float edgeRatio)
{
    const float c0 = (1.0f - centerSize) * 0.5f;
    const float c2 = (edgeRatio - 1.0f) * centerSize + 1.0f;
    const float loBound = c0 * (centerShift + 1.0f) / c2;
    const float hiBound = c0 * (centerShift - 1.0f) / c2 + 1.0f;
    return CompressAxis((loBound + hiBound) * 0.5f, centerSize, centerShift, edgeRatio);
}

struct AxisParams
{
    float centerSize;
    float targetSize;
    float edgeRatio;
    float gridStep;
};

AxisParams AxisX(const FoveationLayout& layout)
{
    const float target = static_cast<float>(layout.targetEyeWidth);
    const float edgeSize = target - layout.parameters.centerSizeX * target;
    return {layout.parameters.centerSizeX, target, layout.parameters.edgeRatioX,
            layout.parameters.edgeRatioX * 2.0f / edgeSize};
}

AxisParams AxisY(const FoveationLayout& layout)
{
    const float target = static_cast<float>(layout.targetEyeHeight);
    const float edgeSize = target - layout.parameters.centerSizeY * target;
    return {layout.parameters.centerSizeY, target, layout.parameters.edgeRatioY,
            layout.parameters.edgeRatioY * 2.0f / edgeSize};
}

TrackingPacket MakeGazePacket(float dirX, float dirY, float dirZ,
                              float fovLeft, float fovRight, float fovUp, float fovDown)
{
    TrackingPacket packet = {};
    packet.trackingFlags = TRACKING_FLAG_EYE_GAZE_ACTIVE;
    packet.gazeDirection[0] = dirX;
    packet.gazeDirection[1] = dirY;
    packet.gazeDirection[2] = dirZ;
    packet.eyeFov[0] = fovLeft;
    packet.eyeFov[1] = fovRight;
    packet.eyeFov[2] = fovUp;
    packet.eyeFov[3] = fovDown;
    return packet;
}

constexpr float kAsymFovLeft = -0.9076f;  // -52 degrees
constexpr float kAsymFovRight = 0.7330f;  // +42 degrees
constexpr float kAsymFovUp = 0.8727f;     // +50 degrees
constexpr float kAsymFovDown = -0.8378f;  // -48 degrees

const FoveationPreset kPresets[] = {FoveationPreset::Light, FoveationPreset::Medium,
                                    FoveationPreset::High};

} // namespace

TEST_CASE("AlignCenterShift is symmetric, idempotent, and exact at the endpoints",
          "[foveation][gaze]")
{
    for (const FoveationPreset preset : kPresets)
    {
        const FoveationLayout layout = CalculateFoveationLayout(2016, 1760, preset);
        for (const AxisParams& axis : {AxisX(layout), AxisY(layout)})
        {
            // The grid arithmetic passes through a non-representable centerSize, so the
            // endpoints land within an ulp of exact; the clamp holds |shift| <= 1.
            REQUIRE(std::fabs(AlignCenterShift(1.0f, axis.centerSize, axis.targetSize,
                                               axis.edgeRatio) - 1.0f) <= 1e-6f);
            REQUIRE(std::fabs(AlignCenterShift(-1.0f, axis.centerSize, axis.targetSize,
                                               axis.edgeRatio) + 1.0f) <= 1e-6f);
            for (int i = -100; i <= 100; ++i)
            {
                const float shift = static_cast<float>(i) / 100.0f;
                const float aligned =
                    AlignCenterShift(shift, axis.centerSize, axis.targetSize, axis.edgeRatio);
                // std::ceil here biased every off-grid value toward +1; round-to-nearest must
                // treat both signs identically.
                REQUIRE(aligned ==
                        -AlignCenterShift(-shift, axis.centerSize, axis.targetSize,
                                          axis.edgeRatio));
                REQUIRE(std::fabs(aligned) <= 1.0f);
                REQUIRE(std::fabs(aligned - shift) <= axis.gridStep * 0.5f + 1e-5f);
                REQUIRE(AlignCenterShift(aligned, axis.centerSize, axis.targetSize,
                                         axis.edgeRatio) == aligned);
            }
        }
    }
}

TEST_CASE("Server warp and client un-warp decode identical shifts from the wire bytes",
          "[foveation][gaze]")
{
    for (const FoveationPreset preset : kPresets)
    {
        const FoveationLayout layout = CalculateFoveationLayout(2144, 2144, preset);
        const AxisParams axisX = AxisX(layout);
        const AxisParams axisY = AxisY(layout);
        for (int q = -127; q <= 127; ++q)
        {
            const int8_t quantized = static_cast<int8_t>(q);
            // The exact expression VideoEncoder::EncodeInternal warps with.
            const float serverShiftX = DecodeCenterShift(quantized, axisX.centerSize,
                                                         axisX.targetSize, axisX.edgeRatio);
            const float serverShiftY = DecodeCenterShift(quantized, axisY.centerSize,
                                                         axisY.targetSize, axisY.edgeRatio);
            // The client-side reconstruction from the same bytes.
            FoveationLayout clientLayout = layout;
            ApplyQuantizedCenterShift(clientLayout, quantized, quantized);
            REQUIRE(clientLayout.parameters.centerShiftX == serverShiftX);
            REQUIRE(clientLayout.parameters.centerShiftY == serverShiftY);

            // The float-input convenience wrapper must agree: quantizing is its first step.
            const float shift = DequantizeCenterShift(quantized);
            FoveationLayout gazeLayout = layout;
            ApplyGazeCenterShift(gazeLayout, shift, shift);
            REQUIRE(gazeLayout.parameters.centerShiftX == serverShiftX);
            REQUIRE(gazeLayout.parameters.centerShiftY == serverShiftY);
        }
    }
}

TEST_CASE("QuantizeCenterShift clamps, rejects NaN, and round-trips within one step",
          "[foveation][gaze]")
{
    REQUIRE(QuantizeCenterShift(std::numeric_limits<float>::quiet_NaN()) == 0);
    REQUIRE(QuantizeCenterShift(std::numeric_limits<float>::infinity()) == 127);
    REQUIRE(QuantizeCenterShift(-std::numeric_limits<float>::infinity()) == -127);
    REQUIRE(QuantizeCenterShift(2.0f) == 127);
    REQUIRE(QuantizeCenterShift(-2.0f) == -127);
    REQUIRE(QuantizeCenterShift(0.0f) == 0);
    for (int i = -100; i <= 100; ++i)
    {
        const float shift = static_cast<float>(i) / 100.0f;
        REQUIRE(std::fabs(DequantizeCenterShift(QuantizeCenterShift(shift)) - shift) <=
                0.5f / 127.0f + 1e-6f);
    }
    for (int q = -127; q <= 127; ++q)
    {
        const int8_t quantized = static_cast<int8_t>(q);
        REQUIRE(QuantizeCenterShift(DequantizeCenterShift(quantized)) == quantized);
    }
}

TEST_CASE("Gaze steering lands the foveal centre on the fixation point", "[foveation][gaze]")
{
    const FoveationLayout layout = CalculateFoveationLayout(2016, 1760, FoveationPreset::Medium);
    const AxisParams axisX = AxisX(layout);
    const AxisParams axisY = AxisY(layout);

    const float tanLeft = std::tan(kAsymFovLeft);
    const float tanRight = std::tan(kAsymFovRight);
    const float tanUp = std::tan(kAsymFovUp);
    const float tanDown = std::tan(kAsymFovDown);
    const float spanX = tanRight - tanLeft;
    const float spanY = tanUp - tanDown;

    // One quantization step plus one alignment step, expressed in source UV.
    const float uvToleranceX =
        (axisX.gridStep + 1.0f / 127.0f) * (1.0f - axisX.centerSize) * 0.5f + 1e-4f;
    const float uvToleranceY =
        (axisY.gridStep + 1.0f / 127.0f) * (1.0f - axisY.centerSize) * 0.5f + 1e-4f;

    for (int i = -4; i <= 4; ++i)
    {
        const float tanGazeX = static_cast<float>(i) * 0.12f;
        const float tanGazeY = static_cast<float>(i) * 0.10f;
        const TrackingPacket packet =
            MakeGazePacket(tanGazeX, tanGazeY, -1.0f,
                           kAsymFovLeft, kAsymFovRight, kAsymFovUp, kAsymFovDown);
        const GazeCenterOffset offset =
            ComputeGazeCenterOffset(packet, axisX.centerSize, axisY.centerSize);
        REQUIRE(offset.valid);

        const float shiftX = DecodeCenterShift(QuantizeCenterShift(offset.x), axisX.centerSize,
                                               axisX.targetSize, axisX.edgeRatio);
        const float shiftY = DecodeCenterShift(QuantizeCenterShift(offset.y), axisY.centerSize,
                                               axisY.targetSize, axisY.edgeRatio);

        // The warp applies one un-mirrored shift to both eye halves, so the best reachable
        // horizontal target is the average of the two (mirrored-FOV) eyes' fixation UVs; the
        // vertical FOV is shared, so the target is the exact fixation row. UV is Y-down, so
        // the row for tan up-positive gaze flips.
        const float uvLeftEye = (tanGazeX - tanLeft) / spanX;
        const float uvRightEye = (tanGazeX + tanRight) / spanX;
        const float expectedUvX = (uvLeftEye + uvRightEye) * 0.5f;
        const float expectedUvY = (tanUp - tanGazeY) / spanY;

        REQUIRE(std::fabs(FovealCenterSourceUv(axisX.centerSize, shiftX, axisX.edgeRatio) -
                          expectedUvX) <= uvToleranceX);
        REQUIRE(std::fabs(FovealCenterSourceUv(axisY.centerSize, shiftY, axisY.edgeRatio) -
                          expectedUvY) <= uvToleranceY);
    }

    // Gaze far outside the steerable range pins the sharp region flush against the frame edge.
    const TrackingPacket farRight = MakeGazePacket(5.0f, 0.0f, -1.0f, kAsymFovLeft,
                                                   kAsymFovRight, kAsymFovUp, kAsymFovDown);
    const GazeCenterOffset extreme =
        ComputeGazeCenterOffset(farRight, axisX.centerSize, axisY.centerSize);
    REQUIRE(extreme.valid);
    REQUIRE(extreme.x == 1.0f);
    REQUIRE(std::fabs(FovealCenterSourceUv(axisX.centerSize, 1.0f, axisX.edgeRatio) -
                      (1.0f - axisX.centerSize * 0.5f)) <= 1e-5f);
}

TEST_CASE("Gaze offset sign conventions match the encoder's Y-down UV space",
          "[foveation][gaze]")
{
    const TrackingPacket right = MakeGazePacket(0.3f, 0.0f, -1.0f, kAsymFovLeft, kAsymFovRight,
                                                kAsymFovUp, kAsymFovDown);
    const GazeCenterOffset rightOffset = ComputeGazeCenterOffset(right, 0.45f, 0.40f);
    REQUIRE(rightOffset.valid);
    REQUIRE(rightOffset.x > 0.0f);

    const TrackingPacket up = MakeGazePacket(0.0f, 0.3f, -1.0f, kAsymFovLeft, kAsymFovRight,
                                             kAsymFovUp, kAsymFovDown);
    const GazeCenterOffset upOffset = ComputeGazeCenterOffset(up, 0.45f, 0.40f);
    REQUIRE(upOffset.valid);
    REQUIRE(upOffset.y < 0.0f);

    // With no reported FOV the fallback is symmetric, so straight-ahead gaze stays centred.
    const TrackingPacket ahead = MakeGazePacket(0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const GazeCenterOffset aheadOffset = ComputeGazeCenterOffset(ahead, 0.45f, 0.40f);
    REQUIRE(aheadOffset.valid);
    REQUIRE(aheadOffset.x == 0.0f);
    REQUIRE(aheadOffset.y == 0.0f);
}

TEST_CASE("ComputeGazeCenterOffset rejects inactive, degenerate, and non-finite input",
          "[foveation][gaze]")
{
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    const float kInf = std::numeric_limits<float>::infinity();

    TrackingPacket inactive = MakeGazePacket(0.0f, 0.0f, -1.0f, kAsymFovLeft, kAsymFovRight,
                                             kAsymFovUp, kAsymFovDown);
    inactive.trackingFlags = 0;
    REQUIRE_FALSE(ComputeGazeCenterOffset(inactive, 0.45f, 0.40f).valid);

    const TrackingPacket behind = MakeGazePacket(0.2f, 0.1f, 1.0f, kAsymFovLeft, kAsymFovRight,
                                                 kAsymFovUp, kAsymFovDown);
    REQUIRE_FALSE(ComputeGazeCenterOffset(behind, 0.45f, 0.40f).valid);

    for (int component = 0; component < 3; ++component)
    {
        TrackingPacket poisoned = MakeGazePacket(0.0f, 0.0f, -1.0f, kAsymFovLeft, kAsymFovRight,
                                                 kAsymFovUp, kAsymFovDown);
        poisoned.gazeDirection[component] = kNan;
        REQUIRE_FALSE(ComputeGazeCenterOffset(poisoned, 0.45f, 0.40f).valid);
    }
    for (int angle = 0; angle < 4; ++angle)
    {
        TrackingPacket poisoned = MakeGazePacket(0.0f, 0.0f, -1.0f, kAsymFovLeft, kAsymFovRight,
                                                 kAsymFovUp, kAsymFovDown);
        poisoned.eyeFov[angle] = angle % 2 == 0 ? kNan : kInf;
        REQUIRE_FALSE(ComputeGazeCenterOffset(poisoned, 0.45f, 0.40f).valid);
    }

    // Angles past pi/2 produce an inverted tan span; a hostile packet must not steer.
    const TrackingPacket inverted = MakeGazePacket(0.0f, 0.0f, -1.0f, -1.6f, 1.6f, kAsymFovUp,
                                                   kAsymFovDown);
    REQUIRE_FALSE(ComputeGazeCenterOffset(inverted, 0.45f, 0.40f).valid);

    // A foveal region covering the whole frame leaves nowhere to steer.
    const TrackingPacket ahead = MakeGazePacket(0.0f, 0.0f, -1.0f, kAsymFovLeft, kAsymFovRight,
                                                kAsymFovUp, kAsymFovDown);
    REQUIRE_FALSE(ComputeGazeCenterOffset(ahead, 1.0f, 0.40f).valid);
    REQUIRE_FALSE(ComputeGazeCenterOffset(ahead, 0.45f, 1.0f).valid);
}

TEST_CASE("GazeCenterFilter converges to gaze, decays without it, and resets", "[foveation][gaze]")
{
    GazeCenterFilter filter;
    GazeCenterOffset target;
    target.x = 1.0f;
    target.y = -1.0f;
    target.valid = true;

    GazeCenterFilter::QuantizedCenter center = {};
    for (int frame = 0; frame < 60; ++frame)
    {
        center = filter.Update(target);
    }
    REQUIRE(center.x == 127);
    REQUIRE(center.y == -127);

    const GazeCenterOffset invalid = {};
    for (int frame = 0; frame < 60; ++frame)
    {
        center = filter.Update(invalid);
    }
    // Gaze dropout (blink, tracking loss, client stops reporting) must drift the centre home,
    // not freeze it at the last fixation.
    REQUIRE(center.x == 0);
    REQUIRE(center.y == 0);

    for (int frame = 0; frame < 60; ++frame)
    {
        filter.Update(target);
    }
    filter.Reset();
    center = filter.Update(invalid);
    REQUIRE(center.x == 0);
    REQUIRE(center.y == 0);
}
