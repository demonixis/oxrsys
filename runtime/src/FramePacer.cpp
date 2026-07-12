// SPDX-License-Identifier: MPL-2.0

#include "FramePacer.h"

#include "RuntimePlatform.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace
{

int64_t WrapToHalfPeriod(int64_t valueNs, int64_t periodNs)
{
    int64_t wrapped = valueNs % periodNs;

    if (wrapped > periodNs / 2)
    {
        wrapped -= periodNs;
    }
    else if (wrapped < -periodNs / 2)
    {
        wrapped += periodNs;
    }

    return wrapped;
}

} // namespace

FramePacer::~FramePacer()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timerRunning_ = false;
    }

    if (pacingThread_.joinable())
    {
        pacingThread_.join();
    }
}

int64_t FramePacer::SnapTickAfter(int64_t tickNs, int64_t minimumNs, int64_t periodNs)
{
    if (tickNs > minimumNs)
    {
        return tickNs;
    }

    const int64_t behindNs = minimumNs - tickNs;

    return tickNs + (behindNs / periodNs + 1) * periodNs;
}

void FramePacer::SetNominalPeriod(int64_t periodNs)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (periodNs == nominalPeriodNs_)
    {
        return;
    }

    nominalPeriodNs_ = periodNs;
    timelinePeriodNs_ = 0;
    timelineLocked_ = false;
}

void FramePacer::OnClientTiming(uint32_t sessionEpoch,int64_t clientPredictedDisplayNs, int64_t clientPeriodNs)
{
    if (clientPeriodNs <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (sessionEpoch != sessionEpoch_)
    {
        sessionEpoch_ = sessionEpoch;
        offsetEstimator_.Reset();
        timelineLocked_ = false;
        timelinePeriodNs_ = 0;
        ResetLearnedStateLocked();
    }

    if (!offsetEstimator_.IsStable())
    {
        return;
    }

    const int64_t nowNs = oxrsys::runtime_platform::SteadyNowNs();
    const int64_t mappedDisplayNs = clientPredictedDisplayNs - offsetEstimator_.GetOffsetNs();

    lastTimingUpdateNs_ = nowNs;

    if (!timelineLocked_ || nextDisplayTickNs_ == 0)
    {
        timelinePeriodNs_ = clientPeriodNs;
        nextDisplayTickNs_ = SnapTickAfter(mappedDisplayNs, nowNs + renderLeadNs_, timelinePeriodNs_);
        timelineLocked_ = true;

        spdlog::info(
            "FramePacer: timeline locked (period={}ns offset={:.3f}ms firstTickIn={:.2f}ms)",
            timelinePeriodNs_,
            static_cast<double>(offsetEstimator_.GetOffsetNs()) / 1.0e6,
            static_cast<double>(nextDisplayTickNs_ - nowNs) / 1.0e6);

        return;
    }

    timelinePeriodNs_ = static_cast<int64_t>(std::lerp(
        static_cast<double>(timelinePeriodNs_),
        static_cast<double>(clientPeriodNs),
        PeriodGain));

    const int64_t phaseErrorNs = WrapToHalfPeriod(mappedDisplayNs - nextDisplayTickNs_, timelinePeriodNs_);

    const int64_t phaseStepNs = std::clamp(
        static_cast<int64_t>(PhaseGain * static_cast<double>(phaseErrorNs)),
        -MaxPhaseStepNs, MaxPhaseStepNs);

    nextDisplayTickNs_ += phaseStepNs;
}

void FramePacer::OnTimesyncSample(int64_t serverSendNs, int64_t clientTimeNs, int64_t serverReceiveNs)
{
    offsetEstimator_.AddSample(serverSendNs, clientTimeNs, serverReceiveNs);
}

bool FramePacer::ClaimTimesyncQuerySlot(int64_t nowNs)
{
    return offsetEstimator_.ClaimQuerySlot(nowNs);
}

void FramePacer::OnFrameFeedback(const FeedbackSample& sample, int64_t nowNs)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const int64_t periodNs = GetPeriodNsLocked();

    if (periodNs <= 0)
    {
        return;
    }

    const int64_t acquireSlackNs = sample.acquireSlackNs;

    if (sample.fresh)
    {
        if (!sample.slackValid)
        {
            unattributedFramesWindow_++;
        }
        else if (acquireSlackNs >= 0)
        {
            // The render lead at feedback time differs from the lead at release
            // by microseconds at most, so the required lead derived from this
            // sample is independent of the current lead
            const int64_t requiredLeadNs = std::max(renderLeadNs_ - acquireSlackNs, MinLeadFloorNs);

            requiredLeadRing_[requiredLeadRingIndex_] = requiredLeadNs;
            requiredLeadRingIndex_ = (requiredLeadRingIndex_ + 1) % RequiredLeadRingSize;
            requiredLeadRingCount_ = std::min(requiredLeadRingCount_ + 1, RequiredLeadRingSize);
            samplesSinceFloorRecompute_++;
        }
        else if (-acquireSlackNs < StallGatePeriods * periodNs)
        {
            // The gap criterion slides with each miss, so a burst can never
            // split itself on a fixed window boundary
            if (lastLeadLimitedMissNs_ != 0 && nowNs - lastLeadLimitedMissNs_ >= LeadLimitedMissGapNs)
            {
                leadLimitedMisses_ = 0;
                missLatenessMaxNs_ = 0;
            }

            lastLeadLimitedMissNs_ = nowNs;
            leadLimitedMisses_++;
            missLatenessMaxNs_ = std::max(missLatenessMaxNs_, std::min(-acquireSlackNs, periodNs));
        }
        else
        {
            unattributedFramesWindow_++;
        }
    }

    if (leadLimitedMisses_ >= LeadLimitedMissLimit)
    {
        const int64_t bumpNs = std::clamp(missLatenessMaxNs_ + BumpMarginNs, MinLeadBumpNs, periodNs / 2);

        renderLeadNs_ = std::min(renderLeadNs_ + bumpNs, GetMaxRenderLeadNsLocked());

        spdlog::info(
            "FramePacer: lead bumped (misses={} maxLateness={:.2f}ms bump={:.2f}ms lead={:.2f}ms)",
            leadLimitedMisses_,
            static_cast<double>(missLatenessMaxNs_) / 1.0e6,
            static_cast<double>(bumpNs) / 1.0e6,
            static_cast<double>(renderLeadNs_) / 1.0e6);

        if (lastLeadBumpNs_ != 0 && nowNs - lastLeadBumpNs_ < StandingMarginMemoryNs)
        {
            standingMarginNs_ = std::clamp(missLatenessMaxNs_ + BumpMarginNs, standingMarginNs_, periodNs);

            spdlog::info(
                "FramePacer: recurring lead limited misses (standingMargin={:.2f}ms)",
                static_cast<double>(standingMarginNs_) / 1.0e6);
        }

        lastLeadBumpNs_ = nowNs;
        leadLimitedMisses_ = 0;
        missLatenessMaxNs_ = 0;
    }

    const size_t warmupSamples = static_cast<size_t>(FloorWarmupNs / periodNs);

    if (samplesSinceFloorRecompute_ >= FloorRecomputeInterval && requiredLeadRingCount_ >= warmupSamples)
    {
        samplesSinceFloorRecompute_ = 0;

        int64_t ringMaxNs = 0;

        for (size_t index = 0; index < requiredLeadRingCount_; index++)
        {
            ringMaxNs = std::max(ringMaxNs, requiredLeadRing_[index]);
        }

        const bool firstEstimate = !leadFloorValid_;

        leadFloorNs_ = std::min(ringMaxNs + FloorMarginNs, GetMaxRenderLeadNsLocked());
        leadFloorValid_ = true;

        if (firstEstimate)
        {
            spdlog::info(
                "FramePacer: lead floor learned (floor={:.2f}ms samples={})",
                static_cast<double>(leadFloorNs_) / 1.0e6, requiredLeadRingCount_);
        }
    }

    if (leadFloorValid_)
    {
        const int64_t decayNs = StandingMarginDecayPerSecondNs * periodNs / 1'000'000'000;
        standingMarginNs_ = std::max<int64_t>(standingMarginNs_ - decayNs, 0);

        const int64_t slewNs = LeadSlewPerSecondNs * periodNs / 1'000'000'000;
        renderLeadNs_ = std::max(renderLeadNs_ - slewNs, GetEffectiveFloorNsLocked());
    }
}

void FramePacer::OnDisplayLag(int64_t displayLagNs)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!timelineLocked_)
    {
        return;
    }

    if (std::abs(displayLagNs) < GetPeriodNsLocked() / 4)
    {
        displayOffsetSettleStreak_ = std::min(displayOffsetSettleStreak_ + 1, GetSettleStreakLimitLocked());
    }
    else
    {
        displayOffsetSettleStreak_ = 0;
    }

    const int64_t stepNs = std::clamp(
        static_cast<int64_t>(DisplayOffsetGain * static_cast<double>(displayLagNs)),
        -MaxDisplayOffsetStepNs, MaxDisplayOffsetStepNs);

    displayOffsetNs_ = std::clamp<int64_t>(displayOffsetNs_ + stepNs, -GetPeriodNsLocked(), GetPeriodNsLocked() * MaxDisplayOffsetPeriods);
}

int64_t FramePacer::GetCurrentTargetDisplayClientNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!timelineLocked_ || releasedDisplayNs_ == 0 || !offsetEstimator_.IsStable())
    {
        return 0;
    }

    return releasedDisplayNs_ + displayOffsetNs_ + offsetEstimator_.GetOffsetNs();
}

int64_t FramePacer::GetDisplayOffsetNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return displayOffsetNs_;
}

int64_t FramePacer::GetSettledTargetDisplayClientNs() const
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (displayOffsetSettleStreak_ < GetSettleStreakLimitLocked())
        {
            return 0;
        }
    }

    return GetCurrentTargetDisplayClientNs();
}

FramePacer::Release FramePacer::WaitForRelease(int64_t nowNs)
{
    std::unique_lock<std::mutex> lock(mutex_);

    const int64_t periodNs = GetPeriodNsLocked();

    if (periodNs <= 0)
    {
        return {nowNs, 0};
    }

    EnsureTimerLocked(nowNs);

    const auto releaseReady = [this] { return releasedTickIndex_ > consumedTickIndex_; };

    // Four periods of grace so ordinary wake jitter never synthesizes a tick
    if (!released_.wait_for(lock, std::chrono::nanoseconds(periodNs * 4), releaseReady))
    {
        releasedTickIndex_++;
        releasedDisplayNs_ = nextDisplayTickNs_;
        nextDisplayTickNs_ += periodNs;

        // Re-arm so the pacing thread does not release again for the tick
        // this synthesis just consumed
        ArmTimerLocked(oxrsys::runtime_platform::SteadyNowNs());

        spdlog::warn("FramePacer: release timeout, synthesized tick");
    }

    consumedTickIndex_ = releasedTickIndex_;

    return {releasedDisplayNs_, GetPeriodNsLocked()};
}

int64_t FramePacer::GetRenderLeadNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return renderLeadNs_;
}

int64_t FramePacer::GetLeadFloorNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return leadFloorValid_ ? leadFloorNs_ : 0;
}

int64_t FramePacer::GetStandingMarginNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return standingMarginNs_;
}

uint32_t FramePacer::GetSettleStreakLimitLocked() const
{
    const int64_t periodNs = GetPeriodNsLocked();

    if (periodNs <= 0)
    {
        return std::numeric_limits<uint32_t>::max();
    }

    return static_cast<uint32_t>(DisplayOffsetSettleNs / periodNs);
}

int64_t FramePacer::GetEffectiveFloorNsLocked() const
{
    if (!leadFloorValid_)
    {
        return MinLeadFloorNs;
    }

    return std::min(leadFloorNs_ + standingMarginNs_, GetMaxRenderLeadNsLocked());
}

int64_t FramePacer::GetPeriodNsLocked() const
{
    if (timelineLocked_ && timelinePeriodNs_ > 0)
    {
        return timelinePeriodNs_;
    }

    return nominalPeriodNs_;
}

int64_t FramePacer::GetMaxRenderLeadNsLocked() const
{
    const int64_t periodNs = GetPeriodNsLocked();

    if (periodNs <= 0)
    {
        return DefaultRenderLeadNs * 2;
    }

    return periodNs * MaxRenderLeadPeriods;
}

void FramePacer::ResetLearnedStateLocked()
{
    leadFloorValid_ = false;
    requiredLeadRingIndex_ = 0;
    requiredLeadRingCount_ = 0;
    samplesSinceFloorRecompute_ = 0;
    displayOffsetNs_ = 0;
    displayOffsetSettleStreak_ = 0;
    standingMarginNs_ = 0;
    lastLeadBumpNs_ = 0;
    leadLimitedMisses_ = 0;
    unattributedFramesWindow_ = 0;
    lastLeadLimitedMissNs_ = 0;
    missLatenessMaxNs_ = 0;
}

void FramePacer::EnsureTimerLocked(int64_t nowNs)
{
    if (timerRunning_)
    {
        return;
    }

    if (nextDisplayTickNs_ == 0)
    {
        nextDisplayTickNs_ = nowNs + renderLeadNs_ + GetPeriodNsLocked();
    }

    timerRunning_ = true;

    ArmTimerLocked(nowNs);

    pacingThread_ = std::thread([this] { PacingThreadMain(); });
}

void FramePacer::ArmTimerLocked(int64_t nowNs)
{
    armedWakeNs_ = nextDisplayTickNs_ - renderLeadNs_;

    if (armedWakeNs_ <= nowNs)
    {
        catchUpReleasesWindow_++;
    }
}

void FramePacer::PacingThreadMain()
{
    int64_t contractPeriodNs = 0;

    for (;;)
    {
        int64_t wakeNs = 0;
        int64_t periodNs = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!timerRunning_)
            {
                return;
            }
            wakeNs = armedWakeNs_;
            periodNs = GetPeriodNsLocked();
        }

        if (periodNs != contractPeriodNs)
        {
            // The realtime contract declares the wake cadence, so it is
            // renewed whenever the timeline period changes
            const bool promoted = oxrsys::runtime_platform::PromoteCurrentThreadToRealtime(periodNs);

            if (contractPeriodNs == 0)
            {
                if (promoted)
                {
                    spdlog::info("FramePacer: pacing thread promoted to realtime");
                }
                else
                {
                    spdlog::warn("FramePacer: realtime promotion rejected, wakes fall back to default scheduling");
                }
            }

            contractPeriodNs = periodNs;
        }

        oxrsys::runtime_platform::WaitUntilSteadyNs(wakeNs);
        OnTimerFired();
    }
}

void FramePacer::OnTimerFired()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!timerRunning_)
    {
        return;
    }

    const int64_t nowNs = oxrsys::runtime_platform::SteadyNowNs();
    const int64_t periodNs = GetPeriodNsLocked();

    if (periodNs <= 0)
    {
        return;
    }

    wakeLatenessWindowMaxNs_ = std::max(wakeLatenessWindowMaxNs_, nowNs - armedWakeNs_);

    if (wakeLatenessLogNs_ == 0)
    {
        wakeLatenessLogNs_ = nowNs;
    }
    else if (nowNs - wakeLatenessLogNs_ >= 10'000'000'000ll)
    {
        spdlog::info(
            "FramePacer: pacing report wakeMax={:.3f}ms timeline={} "
            "lead={:.2f}ms floor={:.2f}ms ({}) margin={:.2f}ms "
            "poseOffset={:.2f}ms "
            "skippedTicks={} catchUpReleases={} unattributedFrames={}",
            static_cast<double>(wakeLatenessWindowMaxNs_) / 1.0e6,
            timelineLocked_ ? "locked" : "free-running",
            static_cast<double>(renderLeadNs_) / 1.0e6,
            static_cast<double>(leadFloorNs_) / 1.0e6,
            leadFloorValid_ ? "learned" : "warming",
            static_cast<double>(standingMarginNs_) / 1.0e6,
            static_cast<double>(displayOffsetNs_) / 1.0e6,
            skippedTicksWindow_,
            catchUpReleasesWindow_,
            unattributedFramesWindow_);

        wakeLatenessWindowMaxNs_ = 0;
        skippedTicksWindow_ = 0;
        catchUpReleasesWindow_ = 0;
        unattributedFramesWindow_ = 0;
        wakeLatenessLogNs_ = nowNs;
    }

    if (timelineLocked_ && nowNs - lastTimingUpdateNs_ > TimingHoldoverNs)
    {
        timelineLocked_ = false;

        spdlog::info("FramePacer: client timing lost, timeline free runs on the nominal period");
    }

    releasedTickIndex_++;
    releasedDisplayNs_ = nextDisplayTickNs_;
    nextDisplayTickNs_ += periodNs;

    // Ticks whose remaining lead is already below the learned floor cannot make
    // their vsync, so the timeline skips ahead of them in one arithmetic step
    const int64_t skipGuardNs = GetEffectiveFloorNsLocked();
    const int64_t snappedTickNs = SnapTickAfter(nextDisplayTickNs_, nowNs + skipGuardNs, periodNs);

    if (snappedTickNs != nextDisplayTickNs_)
    {
        skippedTicksWindow_ += static_cast<uint32_t>((snappedTickNs - nextDisplayTickNs_) / periodNs);
        nextDisplayTickNs_ = snappedTickNs;
    }

    released_.notify_all();

    ArmTimerLocked(nowNs);
}
