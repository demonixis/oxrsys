// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "GraphicsTypes.h"

#include <algorithm>
#include <cstdint>
#include <functional>

// How long the encoder worker waits for an application's snapshot before it
// drops the frame, factored out of VideoEncoder so the switching is
// unit-testable without a GPU.
//
// Two waits. The short one, min(frame period, 16.7 ms), is the wait an
// application that keeps up has always had: a frame that misses it is dropped,
// and the next, newer frame is encoded instead. The long one, up to 100 ms,
// lets an application whose GPU frame outlasts a panel period stream at its
// own rate instead of not at all. The long wait is entered only after a few
// late frames in a row and left only after a long run of on-time ones, so an
// application near the period settles in one mode instead of flapping, and a
// stray late frame from an application that keeps up is dropped, as before,
// not shown late.
namespace oxrsys
{

struct SnapshotWaitSettings
{
    uint64_t shortWaitNs = 0;
    uint64_t longWaitNs = 0;
    uint32_t enterLongAfterLate = 0;    // consecutive late frames that switch to the long wait
    uint32_t leaveLongAfterOnTime = 0;  // consecutive on-time frames that switch back
};

struct SnapshotWaitState
{
    bool longWait = false;
    uint32_t lateStreak = 0;
    uint32_t onTimeStreak = 0;
};

// Three late frames in a row (about 40 ms at 80-90 Hz) to go long; two seconds
// of frames on time to come back.
inline SnapshotWaitSettings SnapshotWaitSettingsForFps(uint32_t fps)
{
    const uint64_t framePeriodNs = 1'000'000'000ull / std::max(fps, 1u);
    SnapshotWaitSettings settings;
    settings.shortWaitNs = std::min<uint64_t>(framePeriodNs, 16'666'667ull);
    settings.longWaitNs = std::max<uint64_t>(framePeriodNs, 100'000'000ull);
    settings.enterLongAfterLate = 3;
    settings.leaveLongAfterOnTime = std::max(fps, 1u) * 2;
    return settings;
}

inline uint64_t SnapshotWaitNs(const SnapshotWaitState& state, const SnapshotWaitSettings& settings)
{
    return state.longWait ? settings.longWaitNs : settings.shortWaitNs;
}

// Records one frame's wait. A frame is late when it was not ready in time, or
// when it took longer than the short wait, i.e. when the short wait would have
// dropped it. Returns true when the mode switched.
inline bool RecordSnapshotWait(SnapshotWaitState& state, const SnapshotWaitSettings& settings,
                               bool ready, uint64_t waitedNs)
{
    const bool late = !ready || waitedNs > settings.shortWaitNs;
    if (late)
    {
        state.onTimeStreak = 0;
        state.lateStreak = std::min<uint32_t>(state.lateStreak + 1, settings.enterLongAfterLate);
        if (!state.longWait && state.lateStreak >= settings.enterLongAfterLate)
        {
            state.longWait = true;
            return true;
        }
        return false;
    }
    state.lateStreak = 0;
    state.onTimeStreak = std::min<uint32_t>(state.onTimeStreak + 1, settings.leaveLongAfterOnTime);
    if (state.longWait && state.onTimeStreak >= settings.leaveLongAfterOnTime)
    {
        state.longWait = false;
        return true;
    }
    return false;
}

enum class SnapshotWaitResult
{
    Ready,
    TimedOut,
    Superseded,
};

// Waits for a frame's snapshots, both eyes against one deadline, so a stereo
// frame waits at most budgetNs in total rather than budgetNs per eye. A frame
// that is ready is then still dropped when isSuperseded says a newer one has
// finished meanwhile, so the wait never leads to encoding stale content.
// waitedNs is the time spent waiting; nowNs reads a nanosecond clock.
template <typename NowNs>
SnapshotWaitResult WaitForFrameSnapshots(const FrameSource& source, bool stereo, uint64_t budgetNs, NowNs nowNs,
                                         const std::function<bool()>& isSuperseded, uint64_t& waitedNs)
{
    const uint64_t startNs = nowNs();
    const uint64_t deadlineNs = startNs + budgetNs;
    bool ready = source.left.sync.WaitForHostReady(budgetNs);
    if (ready && stereo)
    {
        const uint64_t afterLeftNs = nowNs();
        ready = source.right.sync.WaitForHostReady(afterLeftNs < deadlineNs ? deadlineNs - afterLeftNs : 0);
    }
    waitedNs = nowNs() - startNs;
    if (!ready)
    {
        return SnapshotWaitResult::TimedOut;
    }
    if (isSuperseded && isSuperseded())
    {
        return SnapshotWaitResult::Superseded;
    }
    return SnapshotWaitResult::Ready;
}

} // namespace oxrsys
