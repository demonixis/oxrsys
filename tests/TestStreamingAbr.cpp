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
