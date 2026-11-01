// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "Foveation.h"
#include "QuestFoveation.h"

using oxr::quest_foveation::DecompressAxisAndSlope;

namespace
{

// The server's forward warp (runtime/src/VideoEncoder.mm compress_axis), in double.
double CompressAxis(double u, double centerSize, double centerShift, double edgeRatio)
{
    double c0 = (1.0 - centerSize) * 0.5;
    double c1 = (edgeRatio - 1.0) * c0 * (centerShift + 1.0) / edgeRatio;
    double c2 = (edgeRatio - 1.0) * centerSize + 1.0;
    double loBound = c0 * (centerShift + 1.0) / c2;
    double hiBound = c0 * (centerShift - 1.0) / c2 + 1.0;
    double center = u * c2 / edgeRatio + c1;
    double g1 = loBound > 0.0 ? u / loBound : 1.0;
    double g2 = (1.0 - hiBound) > 0.0 ? (1.0 - u) / (1.0 - hiBound) : 1.0;
    if (u < loBound)
    {
        return g1 * center + (1.0 - g1) * u * c2;
    }
    if (u > hiBound)
    {
        return g2 * center + (1.0 - g2) * ((u - 1.0) * c2 + 1.0);
    }
    return center;
}

// Independent oracle: the forward warp inverted by a 60-step bisection. It shares no algebra
// with the closed form under test.
double BisectInverse(double t, double centerSize, double centerShift, double edgeRatio)
{
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 60; ++i)
    {
        double mid = (lo + hi) * 0.5;
        if (CompressAxis(mid, centerSize, centerShift, edgeRatio) < t)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return (lo + hi) * 0.5;
}

struct AxisCase
{
    float centerSize;
    float centerShift;
    float edgeRatio;
};

// Largest |closed form - oracle| over a dense sweep of encoded UVs, plus the largest relative
// error of the returned inverse slope against a central difference of the oracle.
void CheckAxis(const AxisCase& axis, double& maxUvError, double& maxSlopeError)
{
    maxUvError = 0.0;
    maxSlopeError = 0.0;
    const int samples = 4096;
    for (int i = 0; i <= samples; ++i)
    {
        double t = static_cast<double>(i) / samples;
        float invSlope = 0.0f;
        float u = DecompressAxisAndSlope(static_cast<float>(t), axis.centerSize, axis.centerShift,
                                         axis.edgeRatio, invSlope);
        double expected = BisectInverse(t, axis.centerSize, axis.centerShift, axis.edgeRatio);
        maxUvError = std::fmax(maxUvError, std::fabs(u - expected));

        const double h = 1e-4;
        if (t > 2.0 * h && t < 1.0 - 2.0 * h)
        {
            double tPlus = std::fmin(t + h, 1.0);
            double tMinus = std::fmax(t - h, 0.0);
            double expectedInvSlope =
                (BisectInverse(tPlus, axis.centerSize, axis.centerShift, axis.edgeRatio) -
                 BisectInverse(tMinus, axis.centerSize, axis.centerShift, axis.edgeRatio)) /
                (tPlus - tMinus);
            // A piece boundary inside [t-h, t+h] blends two slopes in the difference; skip it.
            double uPlus = BisectInverse(tPlus, axis.centerSize, axis.centerShift, axis.edgeRatio);
            double uMinus = BisectInverse(tMinus, axis.centerSize, axis.centerShift, axis.edgeRatio);
            double c0 = (1.0 - axis.centerSize) * 0.5;
            double c2 = (axis.edgeRatio - 1.0) * axis.centerSize + 1.0;
            double loBound = c0 * (axis.centerShift + 1.0) / c2;
            double hiBound = c0 * (axis.centerShift - 1.0) / c2 + 1.0;
            bool straddles = (uMinus < loBound && uPlus > loBound) ||
                             (uMinus < hiBound && uPlus > hiBound);
            if (!straddles)
            {
                maxSlopeError = std::fmax(maxSlopeError,
                                          std::fabs(invSlope - expectedInvSlope) / expectedInvSlope);
            }
        }
    }
}

AxisCase AxisX(const oxr::protocol::FoveationPresetParameters& p, float shift)
{
    return {p.centerSizeX, shift, p.edgeRatioX};
}

AxisCase AxisY(const oxr::protocol::FoveationPresetParameters& p, float shift)
{
    return {p.centerSizeY, shift, p.edgeRatioY};
}

} // namespace

TEST_CASE("Quest foveation closed-form inverse matches the server warp", "[quest][foveation]")
{
    using oxr::protocol::FoveationPreset;
    for (FoveationPreset preset : {FoveationPreset::Light, FoveationPreset::Medium, FoveationPreset::High})
    {
        oxr::protocol::FoveationPresetParameters params = oxr::protocol::ParametersForPreset(preset);
        for (float shift : {0.0f, 0.1f, -0.1f, 0.4f, -0.4f})
        {
            for (const AxisCase& axis : {AxisX(params, shift), AxisY(params, shift)})
            {
                double maxUvError = 0.0;
                double maxSlopeError = 0.0;
                CheckAxis(axis, maxUvError, maxSlopeError);
                INFO("centerSize " << axis.centerSize << " shift " << axis.centerShift
                                   << " edgeRatio " << axis.edgeRatio);
                // A 4096 px eye has texels of 2.4e-4; stay well under a hundredth of one.
                REQUIRE(maxUvError < 2e-6);
                REQUIRE(maxSlopeError < 1e-3);
            }
        }
    }
}

TEST_CASE("Quest foveation inverse slope places neighbor taps on the warp", "[quest][foveation]")
{
    oxr::protocol::FoveationPresetParameters params =
        oxr::protocol::ParametersForPreset(oxr::protocol::FoveationPreset::High);
    // A target deep in the left edge, where the warp's slope differs most from the center's.
    float invSlope = 0.0f;
    float u = DecompressAxisAndSlope(0.02f, params.centerSizeX, 0.0f, params.edgeRatioX, invSlope);
    double centerInvSlope = params.edgeRatioX /
                            ((params.edgeRatioX - 1.0) * params.centerSizeX + 1.0);
    REQUIRE(std::fabs(invSlope - centerInvSlope) > 0.1 * centerInvSlope);

    const double step = 1.0 / 4096.0;
    double linear = u + step * invSlope;
    double exact = BisectInverse(0.02 + step, params.centerSizeX, 0.0, params.edgeRatioX);
    REQUIRE(std::fabs(linear - exact) < 1e-6);
}
