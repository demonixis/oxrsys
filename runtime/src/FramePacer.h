// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "ClockOffsetEstimator.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <thread>

/**
 * Paces server frame release against the client display clock.
 *
 * Tracking packets carry the client predicted display time and period. The pacer
 * phase locks a server local release timeline to that grid and wakes the app frame
 * loop one render lead ahead of each display tick. Frame feedback closes the loop.
 * The render lead floor is learned from the measured pipeline duration of displayed
 * frames and the lead is raised by the observed lateness when frames miss their tick.
 *
 * Timestamps are server steady clock nanoseconds unless the name says Client.
 * Public methods are thread safe. WaitForRelease is meant for the app frame loop.
 * Releases fire from a dedicated pacing thread that runs under the platform
 * realtime scheduling class when available.
 */
class FramePacer
{
public:
    /** One frame release on the paced timeline. */
    struct Release
    {
        /** Display tick the released frame is aimed at. */
        int64_t displayTimeNs = 0;

        /** Timeline period behind the release, or 0 before any period is known. */
        int64_t periodNs = 0;
    };

    /** One displayed frame outcome reported by the client. */
    struct FeedbackSample
    {
        /** True when the frame was newly displayed rather than reused. */
        bool fresh = false;

        /** True when acquireSlackNs is measured against a carried target slot. */
        bool slackValid = false;

        /**
         * Slack of decode completion against the acquire deadline of the
         * frame's intended display slot. Negative values mean the frame was
         * late by that amount.
         */
        int64_t acquireSlackNs = 0;
    };

    FramePacer() = default;
    ~FramePacer();

    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    /**
     * Returns the first timeline tick strictly after minimumNs.
     *
     * Runs in constant time for any distance between tickNs and minimumNs.
     * periodNs must be positive.
     */
    static int64_t SnapTickAfter(int64_t tickNs, int64_t minimumNs, int64_t periodNs);

    /** Sets the fallback period used until client timing locks the timeline. */
    void SetNominalPeriod(int64_t periodNs);

    /**
     * Feeds one client timing sample from a tracking packet.
     *
     * A changed sessionEpoch resets the clock offset estimator and all
     * learned pacing state. The current render lead survives the reset.
     */
    void OnClientTiming(uint32_t sessionEpoch,int64_t clientPredictedDisplayNs, int64_t clientPeriodNs);

    /** Feeds one timesync round trip into the clock offset estimator. */
    void OnTimesyncSample(int64_t serverSendNs, int64_t clientTimeNs, int64_t serverReceiveNs);

    /**
     * Claims the next timesync query slot.
     *
     * Returns true when the interval since the previous claim has passed.
     * The caller is then expected to send the query.
     */
    bool ClaimTimesyncQuerySlot(int64_t nowNs);

    /** Feeds one frame outcome into the render lead law. */
    void OnFrameFeedback(const FeedbackSample& sample, int64_t nowNs);

    /**
     * Feeds the observed lag between a frame's actual display tick and the
     * tick it was aimed at, both in the client clock. The pose target offset
     * follows this lag so poses are predicted to where frames really display.
     */
    void OnDisplayLag(int64_t displayLagNs);

    /**
     * Returns the client clock display tick the pose should be predicted to,
     * or 0 before the timeline and clock offset are usable.
     */
    int64_t GetCurrentTargetDisplayClientNs() const;

    /**
     * Returns GetCurrentTargetDisplayClientNs() once the pose target servo has
     * settled, otherwise 0. Frames are stamped with this value so the client
     * never rebases slack against a target the servo is still converging on.
     */
    int64_t GetSettledTargetDisplayClientNs() const;

    /** Gets the current pose target offset. */
    int64_t GetDisplayOffsetNs() const;

    /** Blocks the app frame loop until the next release tick. */
    Release WaitForRelease(int64_t nowNs);

    /** Gets the current render lead. */
    int64_t GetRenderLeadNs() const;

    /** Gets the learned render lead floor, or 0 while the estimate is warming up. */
    int64_t GetLeadFloorNs() const;

    /** Gets the standing margin above the floor earned by recurring misses. */
    int64_t GetStandingMarginNs() const;

private:
    // Timeline lock. First order gains settle in about ten ticks and stay
    // stable against the pipeline delayed measurement
    static constexpr double PhaseGain = 0.1;
    static constexpr double PeriodGain = 0.1;
    static constexpr int64_t MaxPhaseStepNs = 500'000;
    static constexpr int64_t TimingHoldoverNs = 500'000'000;

    // Render lead law, driven by the client reported acquire slack. Cleanly
    // displayed frames make the required lead observable as renderLead minus
    // slack, a quantity that cannot exceed the current lead, so the sliding
    // maximum over the ring is poison free by construction. The lead moves as
    // max(lead - slew, floor) per feedback. A rising floor lifts it at once
    // and otherwise it slides down at LeadSlewPerSecondNs. Misses never enter
    // the estimator. They bump the lead immediately by the worst observed
    // lateness once LeadLimitedMissLimit accumulate, up
    // to MaxRenderLeadPeriods display periods, and the bump then slides back
    // to the floor. Misses count toward a bump while their gaps stay under
    // LeadLimitedMissGapNs, a criterion that slides with each miss. Lateness
    // beyond StallGatePeriods marks a delivery stall that more lead cannot
    // fix and is kept out of the law entirely.
    //
    // Bumps that recur within StandingMarginMemoryNs convert the observed
    // lateness into a standing margin above the floor, at most one period.
    // The margin decays at StandingMarginDecayPerSecondNs once the link goes
    // quiet, so chronically bursty transports ride higher while clean links
    // pay nothing.
    //
    // The invariance holds only because the client measures slack against
    // the deadline of the frame's intended display slot, not the slot that
    // happened to latch it. Slack measured against the latching slot mirrors
    // the current lead once frames arrive a whole slot early and the floor
    // ratchets to the cap.

    // Startup lead, deliberately generous until the floor warms
    static constexpr int64_t DefaultRenderLeadNs = 20'000'000;
    static constexpr int64_t MaxRenderLeadPeriods = 3;
    static constexpr int64_t LeadSlewPerSecondNs = 250'000;

    // Three misses separate a trend from a stray outlier
    static constexpr uint32_t LeadLimitedMissLimit = 3;
    static constexpr int64_t LeadLimitedMissGapNs = 10'000'000'000;
    static constexpr int64_t MinLeadBumpNs = 500'000;
    static constexpr int64_t BumpMarginNs = 500'000;
    static constexpr int64_t StallGatePeriods = 2;
    static constexpr int64_t MinLeadFloorNs = 1'000'000;
    static constexpr int64_t FloorMarginNs = 1'000'000;

    // About a minute of history, long enough to remember DVFS and thermal swings
    static constexpr size_t RequiredLeadRingSize = 4096;

    // Amortizes the ring scan, no effect on the law
    static constexpr uint32_t FloorRecomputeInterval = 128;
    static constexpr int64_t FloorWarmupNs = 10'000'000'000;
    static constexpr int64_t StandingMarginMemoryNs = 120'000'000'000;
    static constexpr int64_t StandingMarginDecayPerSecondNs = 16'667;

    // Pose target servo. Follows the observed display lag with a small gain
    // and a bounded step so one outlier cannot yank the pose target. The
    // servo counts as settled after a run of feedbacks with small residual
    // lag, which gates the target carried in video frame headers.
    static constexpr double DisplayOffsetGain = 0.1;
    static constexpr int64_t MaxDisplayOffsetStepNs = 1'000'000;
    static constexpr int64_t MaxDisplayOffsetPeriods = 4;
    static constexpr int64_t DisplayOffsetSettleNs = 500'000'000;

    int64_t GetPeriodNsLocked() const;
    int64_t GetMaxRenderLeadNsLocked() const;
    int64_t GetEffectiveFloorNsLocked() const;
    uint32_t GetSettleStreakLimitLocked() const;
    void ResetLearnedStateLocked();
    void EnsureTimerLocked(int64_t nowNs);
    void ArmTimerLocked(int64_t nowNs);
    void PacingThreadMain();
    void OnTimerFired();

    mutable std::mutex mutex_;
    std::condition_variable released_;

    std::thread pacingThread_;
    bool timerRunning_ = false;

    ClockOffsetEstimator offsetEstimator_;
    uint32_t sessionEpoch_ = 0;

    int64_t nominalPeriodNs_ = 0;
    int64_t timelinePeriodNs_ = 0;
    bool timelineLocked_ = false;
    int64_t lastTimingUpdateNs_ = 0;

    int64_t nextDisplayTickNs_ = 0;
    uint64_t releasedTickIndex_ = 0;
    uint64_t consumedTickIndex_ = 0;
    int64_t releasedDisplayNs_ = 0;
    int64_t armedWakeNs_ = 0;
    int64_t wakeLatenessWindowMaxNs_ = 0;
    int64_t wakeLatenessLogNs_ = 0;
    uint32_t skippedTicksWindow_ = 0;
    uint32_t catchUpReleasesWindow_ = 0;

    int64_t renderLeadNs_ = DefaultRenderLeadNs;
    int64_t displayOffsetNs_ = 0;
    uint32_t displayOffsetSettleStreak_ = 0;
    int64_t leadFloorNs_ = 0;
    bool leadFloorValid_ = false;
    std::array<int64_t, RequiredLeadRingSize> requiredLeadRing_ = {};
    size_t requiredLeadRingIndex_ = 0;
    size_t requiredLeadRingCount_ = 0;
    uint32_t samplesSinceFloorRecompute_ = 0;
    uint32_t leadLimitedMisses_ = 0;
    uint32_t unattributedFramesWindow_ = 0;
    int64_t lastLeadLimitedMissNs_ = 0;
    int64_t missLatenessMaxNs_ = 0;
    int64_t standingMarginNs_ = 0;
    int64_t lastLeadBumpNs_ = 0;
};
