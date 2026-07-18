// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <mutex>

/**
 * Estimates the offset between the client clock and the server steady clock
 * from timesync query round trips.
 *
 * The offset is defined as clientTimeNs minus serverTimeNs. Each sample assumes
 * the client stamped its clock at the midpoint of the round trip. The estimate
 * is the mean offset of the lowest RTT half of the sample ring, so asymmetric
 * network spikes are discarded. All methods are thread safe.
 */
class ClockOffsetEstimator
{
public:
    /**
     * Feeds one timesync round trip into the estimate.
     *
     * Once the ring is full, samples with an RTT above three times the
     * running average are rejected as asymmetric outliers.
     */
    void AddSample(int64_t serverSendNs, int64_t clientTimeNs, int64_t serverReceiveNs)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (serverReceiveNs <= serverSendNs)
        {
            return;
        }

        const int64_t rttNs = serverReceiveNs - serverSendNs;

        if (sampleCount_ >= samples_.size() && averageRttNs_ > 0 && rttNs > averageRttNs_ * 3)
        {
            return;
        }

        Sample& slot = samples_[nextSampleIndex_];
        nextSampleIndex_ = (nextSampleIndex_ + 1) % samples_.size();
        slot.rttNs = rttNs;
        slot.offsetNs = clientTimeNs - (serverSendNs + rttNs / 2);

        if (sampleCount_ < samples_.size())
        {
            sampleCount_++;
        }

        std::array<Sample, SampleRingSize> scratch;
        std::copy_n(samples_.begin(), sampleCount_, scratch.begin());
        const size_t bestHalfCount = std::max<size_t>(sampleCount_ / 2, 1);
        std::nth_element(scratch.begin(), scratch.begin() + (bestHalfCount - 1),
                         scratch.begin() + sampleCount_,
                         [](const Sample& a, const Sample& b) { return a.rttNs < b.rttNs; });

        int64_t offsetSumNs = 0;
        int64_t rttSumNs = 0;

        for (size_t index = 0; index < bestHalfCount; index++)
        {
            offsetSumNs += scratch[index].offsetNs;
            rttSumNs += scratch[index].rttNs;
        }

        const int64_t newOffsetNs = offsetSumNs / static_cast<int64_t>(bestHalfCount);
        averageRttNs_ = rttSumNs / static_cast<int64_t>(bestHalfCount);

        stable_ = sampleCount_ >= MinStableSamples &&
                  std::abs(newOffsetNs - offsetNs_) < StabilityGateNs;
        offsetNs_ = newOffsetNs;
    }

    /**
     * Claims the next timesync query slot.
     *
     * Claims are unthrottled until the ring fills and then spaced one
     * steady interval apart. A true result means the caller is expected
     * to send the query.
     */
    bool ClaimQuerySlot(int64_t nowNs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t intervalNs =
            sampleCount_ < samples_.size() ? 0 : SteadyQueryIntervalNs;

        if (nowNs - lastQueryNs_ < intervalNs)
        {
            return false;
        }

        lastQueryNs_ = nowNs;
        return true;
    }

    /** Returns true once the ring has enough samples and the estimate stopped moving. */
    bool IsStable() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stable_;
    }

    /** Gets the estimated offset, defined as clientTimeNs minus serverTimeNs. */
    int64_t GetOffsetNs() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return offsetNs_;
    }

    /** Discards all samples and returns the estimator to its initial state. */
    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sampleCount_ = 0;
        nextSampleIndex_ = 0;
        offsetNs_ = 0;
        averageRttNs_ = 0;
        lastQueryNs_ = 0;
        stable_ = false;
    }

private:
    struct Sample
    {
        int64_t rttNs = 0;
        int64_t offsetNs = 0;
    };

    static constexpr size_t SampleRingSize = 100;
    static constexpr size_t MinStableSamples = 25;
    static constexpr int64_t StabilityGateNs = 20'000'000;
    static constexpr int64_t SteadyQueryIntervalNs = 100'000'000;

    mutable std::mutex mutex_;
    std::array<Sample, SampleRingSize> samples_ = {};
    size_t sampleCount_ = 0;
    size_t nextSampleIndex_ = 0;
    int64_t offsetNs_ = 0;
    int64_t averageRttNs_ = 0;
    int64_t lastQueryNs_ = 0;
    bool stable_ = false;
};
