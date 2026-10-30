// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "SnapshotWaitPolicy.h"
#include "StreamingFrameQueue.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using oxrsys::RecordSnapshotWait;
using oxrsys::SnapshotWaitNs;
using oxrsys::SnapshotWaitSettings;
using oxrsys::SnapshotWaitSettingsForFps;
using oxrsys::SnapshotWaitResult;
using oxrsys::SnapshotWaitState;
using oxrsys::WaitForFrameSnapshots;

namespace
{
constexpr uint64_t kMs = 1'000'000;

// Fixed thresholds written out here, not taken from SnapshotWaitSettingsForFps,
// so the switching tests do not inherit a mistake in the defaults.
SnapshotWaitSettings TestSettings()
{
    SnapshotWaitSettings settings;
    settings.shortWaitNs = 12'500'000;
    settings.longWaitNs = 100 * kMs;
    settings.enterLongAfterLate = 3;
    settings.leaveLongAfterOnTime = 5;
    return settings;
}

// A clock that moves only when a fake fence is waited on, so the wait tests run
// in no real time.
struct FakeClock
{
    uint64_t nowNs = 0;
};

// A host fence that finishes at doneAtNs on the fake clock. Waiting on it
// advances the clock to doneAtNs, or by the whole timeout if that comes first,
// and records each timeout it was given.
FrameImageSource FakeFencedImage(FakeClock& clock, uint64_t doneAtNs, std::vector<uint64_t>& timeouts)
{
    FrameImageSource source = {};
    source.image = std::make_shared<int>(0);
    source.sync.kind = FrameSyncKind::HostFence;
    source.sync.waitForReady = [&clock, doneAtNs, &timeouts](uint64_t timeoutNs) {
        timeouts.push_back(timeoutNs);
        if (clock.nowNs + timeoutNs < doneAtNs)
        {
            clock.nowNs += timeoutNs;
            return false;
        }
        clock.nowNs = std::max(clock.nowNs, doneAtNs);
        return true;
    };
    return source;
}

SnapshotWaitResult WaitOnFakeClock(const FrameSource& source, FakeClock& clock, uint64_t budgetNs,
                                   const std::function<bool()>& isSuperseded, uint64_t& waitedNs)
{
    return WaitForFrameSnapshots(source, true, budgetNs, [&clock] { return clock.nowNs; }, isSuperseded,
                                 waitedNs);
}
} // namespace

TEST_CASE("Snapshot wait: the defaults are the old short wait and a 100 ms long one", "[snapshot-wait]")
{
    const auto at80 = SnapshotWaitSettingsForFps(80);
    CHECK(at80.shortWaitNs == 12'500'000);
    CHECK(at80.longWaitNs == 100 * kMs);
    CHECK(at80.enterLongAfterLate == 3);
    CHECK(at80.leaveLongAfterOnTime == 160);

    const auto at90 = SnapshotWaitSettingsForFps(90);
    CHECK(at90.shortWaitNs == 11'111'111);

    // Below 60 Hz the short wait is capped at 16.7 ms, as it always was.
    const auto at30 = SnapshotWaitSettingsForFps(30);
    CHECK(at30.shortWaitNs == 16'666'667);
    CHECK(at30.longWaitNs == 100 * kMs);

    // fps 0 must not divide by zero; the long wait never drops under a period.
    const auto at0 = SnapshotWaitSettingsForFps(0);
    CHECK(at0.longWaitNs == 1'000'000'000);
    CHECK(at0.shortWaitNs == 16'666'667);
}

TEST_CASE("Snapshot wait: an encoder starts on the short wait", "[snapshot-wait]")
{
    const SnapshotWaitState state;
    CHECK(SnapshotWaitNs(state, TestSettings()) == 12'500'000);
}

TEST_CASE("Snapshot wait: an app on time never leaves the short wait", "[snapshot-wait]")
{
    const auto settings = TestSettings();
    SnapshotWaitState state;
    for (int i = 0; i < 1000; ++i)
    {
        CHECK_FALSE(RecordSnapshotWait(state, settings, true, 4 * kMs));
    }
    CHECK_FALSE(state.longWait);
}

TEST_CASE("Snapshot wait: two late frames in a row stay short, the third goes long", "[snapshot-wait]")
{
    const auto settings = TestSettings();
    SnapshotWaitState state;
    CHECK_FALSE(RecordSnapshotWait(state, settings, false, 12'500'000));
    CHECK_FALSE(RecordSnapshotWait(state, settings, false, 12'500'000));
    CHECK_FALSE(state.longWait);
    CHECK(RecordSnapshotWait(state, settings, false, 12'500'000));
    CHECK(state.longWait);
    CHECK(SnapshotWaitNs(state, settings) == 100 * kMs);
}

TEST_CASE("Snapshot wait: an on-time frame breaks a late streak", "[snapshot-wait]")
{
    const auto settings = TestSettings();
    SnapshotWaitState state;
    // Late, late, on time, late, late: never three in a row, so never long.
    for (int round = 0; round < 50; ++round)
    {
        CHECK_FALSE(RecordSnapshotWait(state, settings, false, 12'500'000));
        CHECK_FALSE(RecordSnapshotWait(state, settings, false, 12'500'000));
        CHECK_FALSE(RecordSnapshotWait(state, settings, true, 3 * kMs));
    }
    CHECK_FALSE(state.longWait);
}

TEST_CASE("Snapshot wait: a ready frame that took longer than the short wait counts as late", "[snapshot-wait]")
{
    // On the long wait a frame arrives in time for it but past the short wait:
    // the short wait would have dropped it, so it keeps the long wait.
    const auto settings = TestSettings();
    SnapshotWaitState state;
    state.longWait = true;
    for (int i = 0; i < 100; ++i)
    {
        CHECK_FALSE(RecordSnapshotWait(state, settings, true, 13 * kMs));
    }
    CHECK(state.longWait);
    // Exactly the short wait is on time.
    SnapshotWaitState fresh;
    RecordSnapshotWait(fresh, settings, true, 12'500'000);
    CHECK(fresh.lateStreak == 0);
    CHECK(fresh.onTimeStreak == 1);
}

TEST_CASE("Snapshot wait: the long wait is left only after a full run of on-time frames", "[snapshot-wait]")
{
    const auto settings = TestSettings();
    SnapshotWaitState state;
    state.longWait = true;
    for (int i = 0; i < 4; ++i)
    {
        CHECK_FALSE(RecordSnapshotWait(state, settings, true, 5 * kMs));
    }
    CHECK(state.longWait);
    // One late frame restarts the count.
    CHECK_FALSE(RecordSnapshotWait(state, settings, true, 20 * kMs));
    for (int i = 0; i < 4; ++i)
    {
        CHECK_FALSE(RecordSnapshotWait(state, settings, true, 5 * kMs));
    }
    CHECK(state.longWait);
    CHECK(RecordSnapshotWait(state, settings, true, 5 * kMs));
    CHECK_FALSE(state.longWait);
    CHECK(SnapshotWaitNs(state, settings) == 12'500'000);
}

TEST_CASE("Snapshot wait: an app near the period settles long and does not flap", "[snapshot-wait]")
{
    // Half the frames late, alternating in pairs: it reaches three late in a
    // row at once and then never gets 160 on time in a row.
    const auto settings = SnapshotWaitSettingsForFps(80);
    SnapshotWaitState state;
    int switches = 0;
    for (int i = 0; i < 80 * 600; ++i)
    {
        const bool late = (i % 7) < 3 || (i % 2 == 0);
        if (RecordSnapshotWait(state, settings, true, late ? 13 * kMs : 12 * kMs))
        {
            ++switches;
        }
    }
    CHECK(switches == 1);
    CHECK(state.longWait);
}

TEST_CASE("Snapshot wait: a slow app that recovers comes back to the short wait once", "[snapshot-wait]")
{
    const auto settings = SnapshotWaitSettingsForFps(90);
    SnapshotWaitState state;
    int switches = 0;
    // A loading screen at 27 fps (35.8 ms GPU frame), then 10 s of a normal app.
    for (int i = 0; i < 300; ++i)
    {
        switches += RecordSnapshotWait(state, settings, i >= 3, state.longWait ? 36 * kMs : 11'111'111) ? 1 : 0;
    }
    CHECK(state.longWait);
    for (int i = 0; i < 900; ++i)
    {
        switches += RecordSnapshotWait(state, settings, true, 6 * kMs) ? 1 : 0;
    }
    CHECK_FALSE(state.longWait);
    CHECK(switches == 2);
}

TEST_CASE("Snapshot wait: streaks saturate instead of wrapping", "[snapshot-wait]")
{
    const auto settings = TestSettings();
    SnapshotWaitState state;
    for (int i = 0; i < 100'000; ++i)
    {
        RecordSnapshotWait(state, settings, true, 1 * kMs);
    }
    CHECK(state.onTimeStreak == settings.leaveLongAfterOnTime);
    for (int i = 0; i < 100'000; ++i)
    {
        RecordSnapshotWait(state, settings, false, 100 * kMs);
    }
    CHECK(state.lateStreak == settings.enterLongAfterLate);
    CHECK(state.longWait);
}

TEST_CASE("Snapshot wait: both eyes share one deadline", "[snapshot-wait]")
{
    FakeClock clock;
    std::vector<uint64_t> leftTimeouts;
    std::vector<uint64_t> rightTimeouts;
    FrameSource frame = {};
    // Each eye alone would fit the 100 ms budget; together they do not.
    frame.left = FakeFencedImage(clock, 70 * kMs, leftTimeouts);
    frame.right = FakeFencedImage(clock, 140 * kMs, rightTimeouts);

    uint64_t waitedNs = 0;
    CHECK(WaitOnFakeClock(frame, clock, 100 * kMs, {}, waitedNs) == SnapshotWaitResult::TimedOut);
    CHECK(waitedNs <= 100 * kMs);
    CHECK(clock.nowNs == 100 * kMs);
    REQUIRE(leftTimeouts.size() == 1);
    CHECK(leftTimeouts[0] == 100 * kMs);
    REQUIRE(rightTimeouts.size() == 1);
    CHECK(rightTimeouts[0] == 30 * kMs);
}

TEST_CASE("Snapshot wait: the right eye gets what the left eye left of the budget", "[snapshot-wait]")
{
    for (const uint64_t leftDoneMs : {0ull, 10ull, 60ull, 99ull, 100ull})
    {
        FakeClock clock;
        std::vector<uint64_t> leftTimeouts;
        std::vector<uint64_t> rightTimeouts;
        FrameSource frame = {};
        frame.left = FakeFencedImage(clock, leftDoneMs * kMs, leftTimeouts);
        frame.right = FakeFencedImage(clock, 100 * kMs, rightTimeouts);

        uint64_t waitedNs = 0;
        CHECK(WaitOnFakeClock(frame, clock, 100 * kMs, {}, waitedNs) == SnapshotWaitResult::Ready);
        CHECK(waitedNs == 100 * kMs);
        REQUIRE(rightTimeouts.size() == 1);
        CHECK(rightTimeouts[0] == (100 - leftDoneMs) * kMs);
    }
}

TEST_CASE("Snapshot wait: a left eye that times out leaves the right eye unwaited", "[snapshot-wait]")
{
    FakeClock clock;
    std::vector<uint64_t> leftTimeouts;
    std::vector<uint64_t> rightTimeouts;
    FrameSource frame = {};
    frame.left = FakeFencedImage(clock, 500 * kMs, leftTimeouts);
    frame.right = FakeFencedImage(clock, 0, rightTimeouts);

    uint64_t waitedNs = 0;
    CHECK(WaitOnFakeClock(frame, clock, 100 * kMs, {}, waitedNs) == SnapshotWaitResult::TimedOut);
    CHECK(waitedNs == 100 * kMs);
    CHECK(rightTimeouts.empty());
}

TEST_CASE("Snapshot wait: a frame whose successor finished during the wait is not encoded", "[snapshot-wait]")
{
    FakeClock clock;
    std::vector<uint64_t> timeouts;
    StreamingFrameQueue queue;
    queue.Start();

    // Frame 1 finishes at 40 ms. While it is waited on, the application submits
    // frame 2 and its GPU finishes it too.
    FrameSource older = {};
    older.left = FakeFencedImage(clock, 40 * kMs, timeouts);
    older.right = FakeFencedImage(clock, 40 * kMs, timeouts);
    const auto waitOnOlder = older.left.sync.waitForReady;
    older.left.sync.waitForReady = [&](uint64_t timeoutNs) {
        StreamingFrame newer = {};
        newer.frameIndex = 2;
        newer.source.left = FakeFencedImage(clock, 30 * kMs, timeouts);
        newer.source.right = FakeFencedImage(clock, 30 * kMs, timeouts);
        queue.PushLatest(std::move(newer));
        return waitOnOlder(timeoutNs);
    };

    uint64_t waitedNs = 0;
    CHECK(WaitOnFakeClock(older, clock, 100 * kMs, [&] { return queue.HasFinishedPendingFrame(); }, waitedNs) ==
          SnapshotWaitResult::Superseded);
    CHECK(waitedNs == 40 * kMs);
}

TEST_CASE("Snapshot wait: a frame is still encoded when its successor has not finished", "[snapshot-wait]")
{
    // An application whose GPU frame outlasts the panel period always has its
    // next frame submitted before this one is done. Dropping for that would
    // drop every frame; this one is still the newest finished picture.
    FakeClock clock;
    std::vector<uint64_t> timeouts;
    StreamingFrameQueue queue;
    queue.Start();

    FrameSource older = {};
    older.left = FakeFencedImage(clock, 40 * kMs, timeouts);
    older.right = FakeFencedImage(clock, 40 * kMs, timeouts);
    StreamingFrame newer = {};
    newer.frameIndex = 2;
    newer.source.left = FakeFencedImage(clock, 80 * kMs, timeouts);
    newer.source.right = FakeFencedImage(clock, 80 * kMs, timeouts);
    queue.PushLatest(std::move(newer));

    uint64_t waitedNs = 0;
    const auto isSuperseded = [&] { return queue.HasFinishedPendingFrame(); };
    CHECK(WaitOnFakeClock(older, clock, 100 * kMs, isSuperseded, waitedNs) == SnapshotWaitResult::Ready);
    // Checking the newer frame did not wait for it.
    CHECK(clock.nowNs == 40 * kMs);

    queue.Clear();
    clock.nowNs = 0;
    CHECK(WaitOnFakeClock(older, clock, 100 * kMs, isSuperseded, waitedNs) == SnapshotWaitResult::Ready);
}
