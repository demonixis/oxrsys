// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>

namespace oxrsys::streaming_abr
{

enum class Mode
{
    Off,
    Bitrate,
    Full,
};

enum class State
{
    Stable,
    Constrained,
    Recovery,
};

struct Sample
{
    float totalClientLatencyMs = 0.0f;
    float displayedFrameAgeMs = 0.0f;
    uint32_t keyframeRequestsDelta = 0;
    uint32_t videoSendDroppedFramesDelta = 0;
    uint32_t encoderDroppedFramesDelta = 0;
    uint32_t reprojectedFramesDelta = 0;
    uint32_t staleFrameReusesDelta = 0;
    uint32_t renderPoseFallbacksDelta = 0;
};

struct Decision
{
    State state = State::Stable;
    uint32_t targetBitrateMbps = 0;
    bool bitrateChanged = false;
    std::string profile = "balanced";
};

inline Mode ParseMode(const std::string& value)
{
    if (value == "off")
    {
        return Mode::Off;
    }
    if (value == "full")
    {
        return Mode::Full;
    }
    return Mode::Bitrate;
}

inline const char* ToString(Mode mode)
{
    switch (mode)
    {
        case Mode::Off:
            return "off";
        case Mode::Full:
            return "full";
        case Mode::Bitrate:
        default:
            return "bitrate";
    }
}

inline const char* ToString(State state)
{
    switch (state)
    {
        case State::Constrained:
            return "constrained";
        case State::Recovery:
            return "recovery";
        case State::Stable:
        default:
            return "stable";
    }
}

class Controller
{
public:
    void Reset(Mode mode, uint32_t currentBitrateMbps, uint32_t maxBitrateMbps,
               uint32_t refreshRateHz = 72u)
    {
        mode_ = mode;
        maxBitrateMbps_ = std::max(maxBitrateMbps, kMinBitrateMbps);
        currentBitrateMbps_ = std::clamp(currentBitrateMbps, kMinBitrateMbps, maxBitrateMbps_);
        state_ = State::Stable;
        stableWindows_ = 0;
        minimumProfileHoldWindows_ = 0;
        floorStuckWindows_ = 0;
        profile_ = "balanced";
        samples_.clear();
        // reprojectedFramesDelta/staleFrameReusesDelta are counted over a fixed
        // wall-clock reporting interval, not per-frame -- at a higher refresh
        // rate, strictly more frames execute in that same interval, so the same
        // *proportional* smoothness produces a higher *absolute* count. The
        // thresholds below were calibrated against a 72Hz baseline; scale
        // incoming counts back to that baseline so 90Hz+ doesn't get flagged
        // as constrained for behaving identically well at a higher frame rate.
        refreshScale_ = 72.0f / std::max(1.0f, static_cast<float>(refreshRateHz));
    }

    Decision Update(const Sample& sample)
    {
        Decision decision = {};
        decision.targetBitrateMbps = currentBitrateMbps_;
        decision.profile = profile_;

        if (mode_ == Mode::Off)
        {
            state_ = State::Stable;
            decision.state = state_;
            return decision;
        }

        samples_.push_back(sample);
        if (samples_.size() > kWindowSize)
        {
            samples_.pop_front();
        }

        const float averageLatencyMs = Average([](const Sample& item) {
            return item.totalClientLatencyMs;
        });
        const float averageFrameAgeMs = Average([](const Sample& item) {
            return item.displayedFrameAgeMs;
        });
        const float averageReprojectedFrames = Average([this](const Sample& item) {
            return static_cast<float>(item.reprojectedFramesDelta) * refreshScale_;
        });
        const float averageStaleReuses = Average([this](const Sample& item) {
            return static_cast<float>(item.staleFrameReusesDelta) * refreshScale_;
        });
        const float scaledReprojectedFrames = static_cast<float>(sample.reprojectedFramesDelta) * refreshScale_;
        const float scaledStaleReuses = static_cast<float>(sample.staleFrameReusesDelta) * refreshScale_;

        const bool immediateRecovery =
            sample.keyframeRequestsDelta > 0 ||
            sample.videoSendDroppedFramesDelta > 1 ||
            sample.encoderDroppedFramesDelta > 0 ||
            sample.displayedFrameAgeMs >= kRecoveryFrameAgeMs ||
            scaledReprojectedFrames >= static_cast<float>(kRecoveryReprojectedFrames) ||
            scaledStaleReuses >= static_cast<float>(kRecoveryStaleReuses);
        const bool constrained =
            immediateRecovery ||
            averageLatencyMs >= kConstrainedLatencyMs ||
            averageFrameAgeMs >= kConstrainedFrameAgeMs ||
            averageReprojectedFrames >= kConstrainedReprojectedFrames ||
            averageStaleReuses >= kConstrainedStaleReuses ||
            sample.renderPoseFallbacksDelta >= kConstrainedFallbacks;

        state_ = immediateRecovery ? State::Recovery : (constrained ? State::Constrained : State::Stable);

        uint32_t nextBitrate = currentBitrateMbps_;
        if (state_ == State::Recovery || state_ == State::Constrained)
        {
            stableWindows_ = 0;
            const uint32_t numerator = state_ == State::Recovery ? 80u : 90u;
            nextBitrate = std::max(currentBitrateMbps_ * numerator / 100u, kMinBitrateMbps);
            minimumProfileHoldWindows_ = std::max(minimumProfileHoldWindows_, kMinimumProfileHoldWindows);

            // Cutting bitrate only helps if bitrate is actually the cause of the
            // constraint (reproj/staleness/latency). If something else is the real
            // cause -- GPU scheduling jitter, not bandwidth -- repeatedly halving
            // bitrate never fixes it, and once at the floor the controller has no
            // way back: Stable requires the very metrics this state can't improve.
            // Cutting is a one-way ratchet without this escape valve. After sitting
            // at the floor a while with no improvement, periodically probe upward
            // to test whether bitrate was ever the actual constraint.
            if (currentBitrateMbps_ <= kMinBitrateMbps)
            {
                floorStuckWindows_++;
                if (floorStuckWindows_ >= kFloorProbeWindows)
                {
                    nextBitrate = std::min(kMinBitrateMbps * 3u, maxBitrateMbps_);
                    floorStuckWindows_ = 0;
                }
            }
            else
            {
                floorStuckWindows_ = 0;
            }
        }
        else
        {
            floorStuckWindows_ = 0;
            if (minimumProfileHoldWindows_ > 0)
            {
                minimumProfileHoldWindows_--;
            }
            stableWindows_++;
            if (stableWindows_ >= kStableWindowsBeforeIncrease &&
                currentBitrateMbps_ < maxBitrateMbps_)
            {
                const uint32_t increase = std::max(currentBitrateMbps_ / 20u, 1u);
                nextBitrate = std::min(currentBitrateMbps_ + increase, maxBitrateMbps_);
                stableWindows_ = 0;
            }
        }

        if (nextBitrate != currentBitrateMbps_)
        {
            currentBitrateMbps_ = nextBitrate;
            decision.bitrateChanged = true;
        }

        decision.targetBitrateMbps = currentBitrateMbps_;
        decision.state = state_;
        decision.profile = SelectProfile(averageFrameAgeMs, averageReprojectedFrames);
        return decision;
    }

private:
    static constexpr size_t kWindowSize = 5;
    static constexpr uint32_t kMinBitrateMbps = 10;
    static constexpr uint32_t kStableWindowsBeforeIncrease = 5;
    static constexpr uint32_t kFloorProbeWindows = 10;
    static constexpr uint32_t kMinimumProfileHoldWindows = 4;
    static constexpr float kConstrainedLatencyMs = 45.0f;
    static constexpr float kConstrainedFrameAgeMs = 55.0f;
    static constexpr float kRecoveryFrameAgeMs = 85.0f;
    // Measured on-device: this client's "pose" reprojection mode produces a
    // steady reproj/stale baseline of ~12-14 per window *independent of
    // server-side GPU/encode load* (confirmed by varying Mac GPU time 3ms-30ms
    // with zero change in reproj/stale counts) -- the lateness is real but its
    // source is delivery/transport timing (likely the USB-ADB-reverse TCP
    // tunnel), not something bitrate cuts can fix. The old thresholds (4/6)
    // were below this baseline, so the controller permanently misread normal
    // operation as "Constrained" and parked bitrate at the floor. Raised past
    // the observed baseline with headroom so genuine excursions above it still
    // trigger correction.
    static constexpr float kConstrainedReprojectedFrames = 20.0f;
    static constexpr float kConstrainedStaleReuses = 24.0f;
    static constexpr uint32_t kRecoveryReprojectedFrames = 35;
    static constexpr uint32_t kRecoveryStaleReuses = 45;
    static constexpr uint32_t kConstrainedFallbacks = 2;

    template <typename Getter>
    float Average(Getter getter) const
    {
        if (samples_.empty())
        {
            return 0.0f;
        }
        float total = 0.0f;
        for (const Sample& sample : samples_)
        {
            total += getter(sample);
        }
        return total / static_cast<float>(samples_.size());
    }

    std::string SelectProfile(float averageFrameAgeMs, float averageReprojectedFrames)
    {
        if (mode_ != Mode::Full)
        {
            profile_ = "bitrate";
            return profile_;
        }

        if (state_ == State::Recovery)
        {
            profile_ = "wifi_smooth";
            return profile_;
        }
        if (state_ == State::Constrained)
        {
            profile_ = "smooth";
            return profile_;
        }
        if (minimumProfileHoldWindows_ == 0 &&
            averageFrameAgeMs < 30.0f &&
            averageReprojectedFrames < 1.0f &&
            currentBitrateMbps_ >= maxBitrateMbps_ * 9u / 10u)
        {
            profile_ = "quality";
            return profile_;
        }
        profile_ = "balanced";
        return profile_;
    }

    Mode mode_ = Mode::Bitrate;
    State state_ = State::Stable;
    uint32_t currentBitrateMbps_ = 50;
    uint32_t maxBitrateMbps_ = 50;
    uint32_t stableWindows_ = 0;
    uint32_t minimumProfileHoldWindows_ = 0;
    uint32_t floorStuckWindows_ = 0;
    std::string profile_ = "balanced";
    float refreshScale_ = 1.0f;
    std::deque<Sample> samples_;
};

} // namespace oxrsys::streaming_abr
