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
    float targetResolutionScale = 1.0f;
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
               float configuredResolutionScale = 1.0f,
               float minimumResolutionScale = 0.5f)
    {
        mode_ = mode;
        maxBitrateMbps_ = std::max(maxBitrateMbps, kMinBitrateMbps);
        currentBitrateMbps_ = std::clamp(currentBitrateMbps, kMinBitrateMbps, maxBitrateMbps_);
        configuredResolutionScale_ = std::clamp(configuredResolutionScale, 0.25f, 1.0f);
        minimumResolutionScale_ = std::clamp(minimumResolutionScale, 0.25f, configuredResolutionScale_);
        state_ = State::Stable;
        stableWindows_ = 0;
        minimumProfileHoldWindows_ = 0;
        profile_ = "balanced";
        samples_.clear();
    }

    Decision Update(const Sample& sample)
    {
        Decision decision = {};
        decision.targetBitrateMbps = currentBitrateMbps_;
        decision.profile = profile_;
        decision.targetResolutionScale = ProfileResolutionScale(profile_);

        if (mode_ == Mode::Off)
        {
            state_ = State::Stable;
            decision.state = state_;
            decision.targetResolutionScale = configuredResolutionScale_;
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
        const float averageReprojectedFrames = Average([](const Sample& item) {
            return static_cast<float>(item.reprojectedFramesDelta);
        });
        const float averageStaleReuses = Average([](const Sample& item) {
            return static_cast<float>(item.staleFrameReusesDelta);
        });

        const bool immediateRecovery =
            sample.keyframeRequestsDelta > 0 ||
            sample.videoSendDroppedFramesDelta > 1 ||
            sample.encoderDroppedFramesDelta > 0 ||
            sample.displayedFrameAgeMs >= kRecoveryFrameAgeMs ||
            sample.reprojectedFramesDelta >= kRecoveryReprojectedFrames ||
            sample.staleFrameReusesDelta >= kRecoveryStaleReuses;
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
        }
        else
        {
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
        decision.targetResolutionScale = ProfileResolutionScale(decision.profile);
        return decision;
    }

private:
    static constexpr size_t kWindowSize = 5;
    static constexpr uint32_t kMinBitrateMbps = 10;
    static constexpr uint32_t kStableWindowsBeforeIncrease = 5;
    static constexpr uint32_t kMinimumProfileHoldWindows = 4;
    static constexpr float kConstrainedLatencyMs = 45.0f;
    static constexpr float kConstrainedFrameAgeMs = 55.0f;
    static constexpr float kRecoveryFrameAgeMs = 85.0f;
    static constexpr float kConstrainedReprojectedFrames = 4.0f;
    static constexpr float kConstrainedStaleReuses = 6.0f;
    static constexpr uint32_t kRecoveryReprojectedFrames = 12;
    static constexpr uint32_t kRecoveryStaleReuses = 18;
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

    float ProfileResolutionScale(const std::string& profile) const
    {
        if (mode_ != Mode::Full)
        {
            return configuredResolutionScale_;
        }
        if (profile == "wifi_smooth")
        {
            return std::max(minimumResolutionScale_, configuredResolutionScale_ * 0.70f);
        }
        if (profile == "smooth")
        {
            return std::max(minimumResolutionScale_, configuredResolutionScale_ * 0.85f);
        }
        return configuredResolutionScale_;
    }

    Mode mode_ = Mode::Bitrate;
    State state_ = State::Stable;
    uint32_t currentBitrateMbps_ = 50;
    uint32_t maxBitrateMbps_ = 50;
    float configuredResolutionScale_ = 1.0f;
    float minimumResolutionScale_ = 0.5f;
    uint32_t stableWindows_ = 0;
    uint32_t minimumProfileHoldWindows_ = 0;
    std::string profile_ = "balanced";
    std::deque<Sample> samples_;
};

} // namespace oxrsys::streaming_abr
