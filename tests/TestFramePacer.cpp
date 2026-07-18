// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "FramePacer.h"
#include "RuntimePlatform.h"

#include <cstdint>

namespace
{

constexpr int64_t kPeriodNs = 13'888'888;
constexpr int64_t kDefaultLeadNs = 20'000'000;
constexpr int64_t kFloorMarginNs = 1'000'000;

// Drives the feedback loop like a client whose pipeline takes chainNs from
// release to its acquire deadline. Slack co-varies with the current lead
// exactly as it does on a real link, so the required lead the pacer derives
// equals chainNs.
struct FeedbackSimulator
{
    FramePacer& pacer;

    int64_t nowNs = 1'000'000'000;

    void Feed(const FramePacer::FeedbackSample& sample)
    {
        pacer.OnFrameFeedback(sample, nowNs);

        nowNs += kPeriodNs;
    }

    void FeedFreshChain(int64_t chainNs)
    {
        Feed({.fresh = true,
              .slackValid = true,
              .acquireSlackNs = pacer.GetRenderLeadNs() - chainNs});
    }

    void FeedFreshLate(int64_t latenessNs)
    {
        Feed({.fresh = true, .slackValid = true, .acquireSlackNs = -latenessNs});
    }

    void FeedFreshWithUnknownSlack()
    {
        Feed({.fresh = true});
    }

    void FeedStale()
    {
        Feed({});
    }

    void RunChainSeconds(int64_t seconds, int64_t chainNs)
    {
        const int64_t frameCount = seconds * 1'000'000'000 / kPeriodNs;

        for (int64_t frame = 0; frame < frameCount; frame++)
        {
            FeedFreshChain(chainNs);
        }
    }
};

} // namespace

TEST_CASE("SnapTickAfter returns the first tick strictly after the minimum", "[pacer]")
{
    CHECK(FramePacer::SnapTickAfter(100, 50, 10) == 100);
    CHECK(FramePacer::SnapTickAfter(100, 100, 10) == 110);
    CHECK(FramePacer::SnapTickAfter(100, 125, 10) == 130);
    CHECK(FramePacer::SnapTickAfter(100, 130, 10) == 140);

    const int64_t farMinimumNs = 1'000'000'000'000'000'000;
    const int64_t snapped = FramePacer::SnapTickAfter(0, farMinimumNs, kPeriodNs);
    CHECK(snapped > farMinimumNs);
    CHECK(snapped - farMinimumNs <= kPeriodNs);
    CHECK(snapped % kPeriodNs == 0);
}

TEST_CASE("Lead slides down to the learned requirement plus margin", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    const int64_t chainNs = 7'000'000;

    simulator.RunChainSeconds(9, chainNs);
    CHECK(pacer.GetLeadFloorNs() == 0);
    CHECK(pacer.GetRenderLeadNs() == kDefaultLeadNs);

    simulator.RunChainSeconds(2, chainNs);
    CHECK(pacer.GetLeadFloorNs() == chainNs + kFloorMarginNs);
    CHECK(pacer.GetRenderLeadNs() > 19'000'000);

    simulator.RunChainSeconds(60, chainNs);
    CHECK(pacer.GetRenderLeadNs() == chainNs + kFloorMarginNs);
}

TEST_CASE("A rising floor raises the lead without waiting for misses", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    simulator.RunChainSeconds(80, 7'000'000);
    REQUIRE(pacer.GetRenderLeadNs() == 8'000'000);

    const int64_t longerChainNs = 12'000'000;
    simulator.RunChainSeconds(11, longerChainNs);
    CHECK(pacer.GetLeadFloorNs() == longerChainNs + kFloorMarginNs);
    CHECK(pacer.GetRenderLeadNs() == longerChainNs + kFloorMarginNs);
}

TEST_CASE("Three late frames bump the lead by the worst observed lateness", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    const int64_t latenessNs = 3'000'000;

    for (int miss = 0; miss < 3; miss++)
    {
        simulator.FeedFreshChain(7'000'000);
        simulator.FeedStale();
        simulator.FeedFreshLate(latenessNs);
    }

    const int64_t bumpMarginNs = 500'000;
    CHECK(pacer.GetRenderLeadNs() == kDefaultLeadNs + latenessNs + bumpMarginNs);
}

TEST_CASE("Delivery stalls beyond the gate neither bump nor block decay", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    const int64_t chainNs = 7'000'000;
    simulator.RunChainSeconds(11, chainNs);
    REQUIRE(pacer.GetLeadFloorNs() == chainNs + kFloorMarginNs);

    for (int second = 0; second < 60; second++)
    {
        simulator.RunChainSeconds(1, chainNs);
        simulator.FeedStale();
        simulator.FeedStale();
        simulator.FeedStale();
        simulator.FeedFreshLate(3 * kPeriodNs);
    }

    CHECK(pacer.GetRenderLeadNs() == chainNs + kFloorMarginNs);
}

TEST_CASE("Misses without slack measurement do not move the lead", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    for (int miss = 0; miss < 10; miss++)
    {
        simulator.FeedFreshWithUnknownSlack();
        simulator.FeedStale();
    }

    CHECK(pacer.GetRenderLeadNs() == kDefaultLeadNs);
}

TEST_CASE("Chains beyond the default lead converge, capped at three periods", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    SECTION("a 26 ms chain settles at its own floor")
    {
        const int64_t longChainNs = 26'000'000;
        simulator.RunChainSeconds(15, longChainNs);
        CHECK(pacer.GetLeadFloorNs() == longChainNs + kFloorMarginNs);
        CHECK(pacer.GetRenderLeadNs() == longChainNs + kFloorMarginNs);
    }

    SECTION("a 45 ms chain stops exactly at the absolute cap")
    {
        simulator.RunChainSeconds(30, 45'000'000);
        CHECK(pacer.GetRenderLeadNs() == 3 * kPeriodNs);
    }
}

TEST_CASE("Floor remembers a tail for the sliding ring and then releases it", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    simulator.RunChainSeconds(11, 7'000'000);
    REQUIRE(pacer.GetLeadFloorNs() == 8'000'000);

    simulator.RunChainSeconds(11, 11'000'000);
    CHECK(pacer.GetLeadFloorNs() == 12'000'000);

    simulator.RunChainSeconds(40, 7'000'000);
    CHECK(pacer.GetLeadFloorNs() == 12'000'000);

    simulator.RunChainSeconds(30, 7'000'000);
    CHECK(pacer.GetLeadFloorNs() == 8'000'000);
}

TEST_CASE("Display offset converges to the observed display lag", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);

    pacer.OnClientTiming(1, 400'000'000, kPeriodNs);

    for (int sample = 0; sample < 30; sample++)
    {
        const int64_t sendNs = sample * 100'000'000;
        pacer.OnTimesyncSample(sendNs, sendNs + 500'000, sendNs + 1'000'000);
    }
    pacer.OnClientTiming(1, 500'000'000, kPeriodNs);
    REQUIRE(pacer.GetCurrentTargetDisplayClientNs() == 0);

    const int64_t trueLagNs = 5'000'000;

    for (int step = 0; step < 300; step++)
    {
        pacer.OnDisplayLag(trueLagNs - pacer.GetDisplayOffsetNs());
    }
    CHECK(pacer.GetDisplayOffsetNs() > trueLagNs - 200'000);
    CHECK(pacer.GetDisplayOffsetNs() <= trueLagNs);

    for (int step = 0; step < 300; step++)
    {
        pacer.OnDisplayLag(-pacer.GetDisplayOffsetNs());
    }
    CHECK(pacer.GetDisplayOffsetNs() < 200'000);

    const int64_t negativeLagNs = -3'000'000;

    for (int step = 0; step < 300; step++)
    {
        pacer.OnDisplayLag(negativeLagNs - pacer.GetDisplayOffsetNs());
    }
    CHECK(pacer.GetDisplayOffsetNs() < negativeLagNs + 200'000);
    CHECK(pacer.GetDisplayOffsetNs() >= negativeLagNs);
}

TEST_CASE("The pacing thread releases ticks on the grid and joins cleanly", "[pacer]")
{
    using oxrsys::runtime_platform::SteadyNowNs;

    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);

    const FramePacer::Release first = pacer.WaitForRelease(SteadyNowNs());
    const FramePacer::Release second = pacer.WaitForRelease(SteadyNowNs());
    const FramePacer::Release third = pacer.WaitForRelease(SteadyNowNs());

    CHECK(first.periodNs == kPeriodNs);
    CHECK(second.displayTimeNs > first.displayTimeNs);
    CHECK(third.displayTimeNs > second.displayTimeNs);
    CHECK((second.displayTimeNs - first.displayTimeNs) % kPeriodNs == 0);
    CHECK((third.displayTimeNs - second.displayTimeNs) % kPeriodNs == 0);
}

TEST_CASE("A lone bump leaves no standing margin", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    const int64_t chainNs = 7'000'000;
    simulator.RunChainSeconds(80, chainNs);
    REQUIRE(pacer.GetRenderLeadNs() == chainNs + kFloorMarginNs);

    for (int miss = 0; miss < 3; miss++)
    {
        simulator.FeedFreshLate(3'000'000);
    }
    CHECK(pacer.GetStandingMarginNs() == 0);

    simulator.RunChainSeconds(30, chainNs);
    CHECK(pacer.GetRenderLeadNs() == chainNs + kFloorMarginNs);
}

TEST_CASE("Recurring bumps convert lateness into a standing margin that decays "
          "and never exceeds one period",
          "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    const int64_t chainNs = 7'000'000;
    simulator.RunChainSeconds(80, chainNs);
    REQUIRE(pacer.GetRenderLeadNs() == chainNs + kFloorMarginNs);

    for (int miss = 0; miss < 3; miss++)
    {
        simulator.FeedFreshLate(3'000'000);
    }
    simulator.RunChainSeconds(30, chainNs);

    for (int miss = 0; miss < 3; miss++)
    {
        simulator.FeedFreshLate(3'000'000);
    }
    CHECK(pacer.GetStandingMarginNs() > 3'400'000);
    CHECK(pacer.GetStandingMarginNs() <= 3'500'000);

    simulator.RunChainSeconds(30, chainNs);
    CHECK(pacer.GetRenderLeadNs() == pacer.GetLeadFloorNs() + pacer.GetStandingMarginNs());
    CHECK(pacer.GetStandingMarginNs() > 2'800'000);

    simulator.RunChainSeconds(300, chainNs);
    CHECK(pacer.GetStandingMarginNs() == 0);
    CHECK(pacer.GetRenderLeadNs() == chainNs + kFloorMarginNs);

    for (int miss = 0; miss < 3; miss++)
    {
        simulator.FeedFreshLate(20'000'000);
    }
    simulator.RunChainSeconds(5, chainNs);

    for (int miss = 0; miss < 3; miss++)
    {
        simulator.FeedFreshLate(20'000'000);
    }
    CHECK(pacer.GetStandingMarginNs() <= kPeriodNs);
    CHECK(pacer.GetStandingMarginNs() > kPeriodNs - 100'000);
}

TEST_CASE("The frame target is withheld until the pose servo settles", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);

    pacer.OnClientTiming(1, 400'000'000, kPeriodNs);

    for (int sample = 0; sample < 30; sample++)
    {
        const int64_t sendNs = sample * 100'000'000;
        pacer.OnTimesyncSample(sendNs, sendNs + 500'000, sendNs + 1'000'000);
    }
    pacer.OnClientTiming(1, 500'000'000, kPeriodNs);

    pacer.WaitForRelease(oxrsys::runtime_platform::SteadyNowNs());
    REQUIRE(pacer.GetCurrentTargetDisplayClientNs() != 0);
    CHECK(pacer.GetSettledTargetDisplayClientNs() == 0);

    for (int step = 0; step < 40; step++)
    {
        pacer.OnDisplayLag(0);
    }
    CHECK(pacer.GetSettledTargetDisplayClientNs() == pacer.GetCurrentTargetDisplayClientNs());

    pacer.OnDisplayLag(kPeriodNs);
    CHECK(pacer.GetSettledTargetDisplayClientNs() == 0);
}

TEST_CASE("A session epoch change resets the learned floor but keeps the lead", "[pacer]")
{
    FramePacer pacer;
    pacer.SetNominalPeriod(kPeriodNs);
    FeedbackSimulator simulator{pacer};

    simulator.RunChainSeconds(80, 7'000'000);
    REQUIRE(pacer.GetRenderLeadNs() == 8'000'000);
    REQUIRE(pacer.GetLeadFloorNs() == 8'000'000);

    pacer.OnClientTiming(42, simulator.nowNs, kPeriodNs);
    CHECK(pacer.GetLeadFloorNs() == 0);
    CHECK(pacer.GetRenderLeadNs() == 8'000'000);

    simulator.RunChainSeconds(5, 7'000'000);
    CHECK(pacer.GetRenderLeadNs() == 8'000'000);
}
