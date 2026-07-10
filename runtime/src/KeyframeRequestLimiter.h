// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstdint>

namespace oxrsys
{

// Rate-limits client keyframe requests: clients spam IDR requests for several
// seconds around connect/loss, and every accepted request costs a full-size
// frame. Internal forces (connect warmup, GOP cadence, stream reconfigure)
// bypass this by calling VideoEncoder::ForceKeyframe() directly. Thread-safe;
// the window check is a benign race (worst case one extra keyframe).
class KeyframeRequestLimiter
{
public:
    // nowNs: steady-clock nanoseconds. True = forward this request.
    bool Accept(int64_t nowNs)
    {
        if (nowNs - lastAcceptedNs_.load(std::memory_order_relaxed) < kMinIntervalNs)
        {
            return false;
        }
        lastAcceptedNs_.store(nowNs, std::memory_order_relaxed);
        return true;
    }

    static constexpr int64_t kMinIntervalNs = 500'000'000; // 2 accepted requests/sec

private:
    std::atomic<int64_t> lastAcceptedNs_{0};
};

} // namespace oxrsys
