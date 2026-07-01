// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "StreamingAbr.h"

using namespace oxrsys::streaming_abr;

TEST_CASE("Streaming ABR lowers bitrate quickly under constrained client health", "[streaming][abr]")
{
    Controller controller;
    controller.Reset(Mode::Bitrate, 80, 100);

    Sample sample = {};
    sample.totalClientLatencyMs = 52.0f;
    sample.displayedFrameAgeMs = 62.0f;
    sample.reprojectedFramesDelta = 6;

    Decision decision = controller.Update(sample);

    CHECK(decision.state == State::Constrained);
    CHECK(decision.bitrateChanged);
    CHECK(decision.targetBitrateMbps == 72);
    CHECK(decision.profile == "bitrate");
}

TEST_CASE("Streaming ABR enters recovery on loss and avoids oscillating upward immediately", "[streaming][abr]")
{
    Controller controller;
    controller.Reset(Mode::Bitrate, 80, 100);

    Sample loss = {};
    loss.totalClientLatencyMs = 28.0f;
    loss.keyframeRequestsDelta = 1;

    Decision recovery = controller.Update(loss);
    CHECK(recovery.state == State::Recovery);
    CHECK(recovery.targetBitrateMbps == 64);

    Sample stable = {};
    stable.totalClientLatencyMs = 20.0f;
    stable.displayedFrameAgeMs = 20.0f;

    uint32_t bitrate = recovery.targetBitrateMbps;
    for (int i = 0; i < 4; ++i)
    {
        Decision decision = controller.Update(stable);
        CHECK(decision.targetBitrateMbps == bitrate);
        CHECK(!decision.bitrateChanged);
    }

    Decision increase = controller.Update(stable);
    CHECK(increase.state == State::Stable);
    CHECK(increase.bitrateChanged);
    CHECK(increase.targetBitrateMbps > bitrate);
}

TEST_CASE("Streaming ABR off mode does not change bitrate", "[streaming][abr]")
{
    Controller controller;
    controller.Reset(Mode::Off, 80, 100);

    Sample sample = {};
    sample.totalClientLatencyMs = 90.0f;
    sample.displayedFrameAgeMs = 100.0f;
    sample.keyframeRequestsDelta = 1;
    sample.staleFrameReusesDelta = 20;

    Decision decision = controller.Update(sample);

    CHECK(decision.state == State::Stable);
    CHECK(!decision.bitrateChanged);
    CHECK(decision.targetBitrateMbps == 80);
}

TEST_CASE("Streaming ABR escapes the floor when stuck constrained for a non-bitrate reason", "[streaming][abr]")
{
    // Reproduces a real stuck state: GPU/timing jitter (not bandwidth) keeps
    // reprojectedFramesDelta above the constrained threshold no matter how low
    // bitrate goes, so the controller cuts to the floor and -- without an
    // escape valve -- stays there forever, since Stable is unreachable while
    // the non-bitrate-caused symptom persists.
    Controller controller;
    controller.Reset(Mode::Bitrate, 100, 100);

    Sample jittery = {};
    jittery.totalClientLatencyMs = 20.0f;
    jittery.displayedFrameAgeMs = 20.0f;
    jittery.reprojectedFramesDelta = 25; // above kConstrainedReprojectedFrames regardless of bitrate

    Decision decision = {};
    bool reachedFloor = false;
    bool probedAboveFloorAfterReachingIt = false;
    for (int i = 0; i < 30; ++i)
    {
        decision = controller.Update(jittery);
        CHECK(decision.state == State::Constrained);
        if (decision.targetBitrateMbps == 10)
        {
            reachedFloor = true;
        }
        else if (reachedFloor && decision.targetBitrateMbps > 10)
        {
            probedAboveFloorAfterReachingIt = true;
        }
    }

    CHECK(reachedFloor); // confirms it did reach the floor first, as before the fix
    CHECK(probedAboveFloorAfterReachingIt); // ...but didn't stay stuck there permanently
}

TEST_CASE("Streaming ABR scales reprojection thresholds by refresh rate", "[streaming][abr]")
{
    // reprojectedFramesDelta/staleFrameReuses are counted over a fixed
    // wall-clock report interval, so a higher refresh rate naturally produces
    // a higher raw count for the same *proportional* smoothness. A count that
    // would rightly flag 72Hz as constrained should not flag 90Hz as
    // constrained, since 90/72 = 1.25x more frames render in the same window.
    Controller controller72;
    controller72.Reset(Mode::Bitrate, 80, 100, 72);

    Sample sample72 = {};
    sample72.totalClientLatencyMs = 20.0f;
    sample72.displayedFrameAgeMs = 20.0f;
    sample72.reprojectedFramesDelta = 21; // just above the 72Hz-calibrated threshold (20)

    Decision decision72 = controller72.Update(sample72);
    CHECK(decision72.state == State::Constrained);

    Controller controller90;
    controller90.Reset(Mode::Bitrate, 80, 100, 90);

    Sample sample90 = {};
    sample90.totalClientLatencyMs = 20.0f;
    sample90.displayedFrameAgeMs = 20.0f;
    sample90.reprojectedFramesDelta = 21; // same raw count, but proportionally equivalent to ~17 at 72Hz

    Decision decision90 = controller90.Update(sample90);
    CHECK(decision90.state == State::Stable);
    CHECK(!decision90.bitrateChanged);
}

TEST_CASE("Streaming ABR full mode selects smooth profiles from reprojection pressure", "[streaming][abr]")
{
    Controller controller;
    controller.Reset(Mode::Full, 80, 100);

    Sample constrained = {};
    constrained.totalClientLatencyMs = 30.0f;
    constrained.displayedFrameAgeMs = 60.0f;
    constrained.reprojectedFramesDelta = 5;

    Decision smooth = controller.Update(constrained);
    CHECK(smooth.state == State::Constrained);
    CHECK(smooth.profile == "smooth");

    Sample recovery = {};
    recovery.displayedFrameAgeMs = 90.0f;

    Decision wifiSmooth = controller.Update(recovery);
    CHECK(wifiSmooth.state == State::Recovery);
    CHECK(wifiSmooth.profile == "wifi_smooth");
}
