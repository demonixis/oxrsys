// SPDX-License-Identifier: MPL-2.0

#include "PacketPacer.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(__aarch64__) || defined(__arm64__)
#define OXRSYS_PACER_CPU_RELAX() __asm__ __volatile__("yield")
#elif defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define OXRSYS_PACER_CPU_RELAX() _mm_pause()
#else
#define OXRSYS_PACER_CPU_RELAX() ((void)0)
#endif

namespace oxrsys::streaming
{

    uint32_t DatagramsForNal(size_t payloadSize, size_t maxPayload, uint32_t fecGroupSize)
    {
        if (payloadSize == 0 || maxPayload == 0)
        {
            return 0;
        }
        const uint32_t dataPackets = static_cast<uint32_t>((payloadSize + maxPayload - 1) / maxPayload);
        const uint32_t parityPackets =
            fecGroupSize == 0 ? 0 : (dataPackets + fecGroupSize - 1) / fecGroupSize;
        return dataPackets + parityPackets;
    }

    int64_t PacingWindowNs(const PacketPacingConfig& config, bool keyframe, uint32_t refreshRateHz)
    {
        if (config.windowMs <= 0.0f)
        {
            return 0;
        }
        double windowMs = config.windowMs;
        if (keyframe)
        {
            windowMs = config.keyframeWindowMs > 0.0f ? config.keyframeWindowMs : 2.0 * config.windowMs;
        }
        int64_t windowNs = static_cast<int64_t>(windowMs * 1'000'000.0);
        if (refreshRateHz > 0)
        {
            const int64_t capNs = static_cast<int64_t>(kMaxPacingFrameFraction * 1'000'000'000.0 / refreshRateHz);
            windowNs = std::min(windowNs, capNs);
        }
        return std::max<int64_t>(0, windowNs);
    }

    PacketPacer::PacketPacer(int64_t startNs, int64_t windowNs, uint32_t totalPackets)
        : startNs_(startNs), windowNs_(std::max<int64_t>(0, windowNs)), totalPackets_(totalPackets)
    {
    }

    int64_t PacketPacer::DeadlineNs(uint32_t packetIndex) const
    {
        if (windowNs_ <= 0 || totalPackets_ <= 1)
        {
            return startNs_;
        }
        const uint32_t index = std::min(packetIndex, totalPackets_ - 1);
        // 128-bit-safe for any realistic window: window < 1e9, index < 65536.
        return startNs_ + (windowNs_ * static_cast<int64_t>(index)) / totalPackets_;
    }

    int64_t PacketPacer::SpacingNs() const
    {
        return (windowNs_ > 0 && totalPackets_ > 1) ? windowNs_ / totalPackets_ : 0;
    }

    void PacketPacer::WaitForNextPacket(const std::function<bool()>& shouldFlush)
    {
        const uint32_t index = nextPacket_++;
        if (!Enabled() || index == 0 || index >= totalPackets_)
        {
            return;
        }
        const int64_t deadline = DeadlineNs(index);
        if (!WaitUntilNs(deadline, shouldFlush))
        {
            flushed_ = true;
            return;
        }
        maxLatenessNs_ = std::max(maxLatenessNs_, PacingNowNs() - deadline);
    }

    int64_t PacingNowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool WaitUntilNs(int64_t deadlineNs, const std::function<bool()>& shouldFlush)
    {
        for (;;)
        {
            if (shouldFlush && shouldFlush())
            {
                return false;
            }
            const int64_t remaining = deadlineNs - PacingNowNs();
            if (remaining <= 0)
            {
                return true;
            }
            if (remaining > kPacingSpinThresholdNs)
            {
                // Sleep in slices so a flush request is still seen promptly.
                const int64_t sleepNs = std::min<int64_t>(remaining - kPacingSpinThresholdNs, 1'000'000);
                std::this_thread::sleep_for(std::chrono::nanoseconds(sleepNs));
                continue;
            }
            OXRSYS_PACER_CPU_RELAX();
        }
    }

} // namespace oxrsys::streaming
