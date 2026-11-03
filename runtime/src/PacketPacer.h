// SPDX-License-Identifier: MPL-2.0
#pragma once

// Sender-side packet pacing for one encoded video frame.
//
// A frame handed to the socket at once leaves the Mac as a line-rate burst of
// 10-200 datagrams; an access point queue under contention drops the tail of
// such a burst, and the XOR FEC (one parity per FEC_GROUP_SIZE packets) cannot
// repair consecutive losses. Spreading the frame's packets evenly over a few
// milliseconds keeps the instantaneous rate near the stream's average rate.
//
// The schedule is pure arithmetic (PacingWindowNs, PacketPacer::DeadlineNs) so
// it can be unit tested; WaitUntilNs is the only part that touches the clock.

#include <cstdint>
#include <functional>

namespace oxrsys::streaming
{

    struct PacketPacingConfig
    {
        // send_pacing_ms: spread a frame's packets over this window. 0 = burst (no pacing).
        float windowMs = 0.0f;
        // send_pacing_keyframe_ms: window for frames carrying a keyframe NAL. 0 = 2 x windowMs.
        float keyframeWindowMs = 0.0f;
    };

    // Never pace a frame over more than this fraction of the frame period, so the
    // last packet leaves before the next frame is normally ready to send.
    constexpr double kMaxPacingFrameFraction = 0.8;

    // Waits shorter than this are spun; longer ones sleep until this far from the
    // deadline and spin the rest (sleep overshoots by 50-500 us on macOS).
    constexpr int64_t kPacingSpinThresholdNs = 1'000'000;

    // Datagrams one NAL of payloadSize bytes becomes on the wire: ceil(payload / maxPayload)
    // data packets plus one FEC parity packet per fecGroupSize of them (and for the tail
    // group). The same arithmetic SendNalUnit sends by, so the pacer's plan and the sender's
    // count cannot drift apart.
    uint32_t DatagramsForNal(size_t payloadSize, size_t maxPayload, uint32_t fecGroupSize);

    // The window (ns) one frame's packets are spread over: the configured window
    // (keyframe window for keyframes), capped at kMaxPacingFrameFraction of the
    // frame period. 0 means send as one burst.
    int64_t PacingWindowNs(const PacketPacingConfig& config, bool keyframe, uint32_t refreshRateHz);

    class PacketPacer
    {
      public:
        // A pacer for totalPackets packets starting at startNs, spread over windowNs.
        // Packet i is due at startNs + windowNs * i / totalPackets, so packet 0 goes
        // at once and the last one is due one spacing before the window closes.
        PacketPacer(int64_t startNs, int64_t windowNs, uint32_t totalPackets);

        int64_t DeadlineNs(uint32_t packetIndex) const;
        int64_t SpacingNs() const;
        bool Enabled() const
        {
            return windowNs_ > 0 && totalPackets_ > 1 && !flushed_;
        }

        // Wait until the next packet is due, then count it as sent. Returns at once
        // when pacing is off, when flushed, or once more packets were sent than
        // planned. shouldFlush is polled while waiting; when it returns true the
        // remaining packets go as a burst (a newer frame is waiting to be sent).
        void WaitForNextPacket(const std::function<bool()>& shouldFlush = {});

        // Send everything that is left without further waits.
        void Flush()
        {
            flushed_ = true;
        }
        bool Flushed() const
        {
            return flushed_;
        }
        uint32_t PacketsSent() const
        {
            return nextPacket_;
        }
        // Largest (send time - deadline) seen so far, ns: how late the waits woke.
        int64_t MaxLatenessNs() const
        {
            return maxLatenessNs_;
        }

      private:
        int64_t startNs_;
        int64_t windowNs_;
        uint32_t totalPackets_;
        uint32_t nextPacket_ = 0;
        bool flushed_ = false;
        int64_t maxLatenessNs_ = 0;
    };

    // Monotonic clock in ns (same base as std::chrono::steady_clock).
    int64_t PacingNowNs();

    // Block until PacingNowNs() >= deadlineNs: sleep while more than
    // kPacingSpinThresholdNs remains, spin the rest. Returns false (early) if
    // shouldFlush becomes true while waiting.
    bool WaitUntilNs(int64_t deadlineNs, const std::function<bool()>& shouldFlush = {});

} // namespace oxrsys::streaming
