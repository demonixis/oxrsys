// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <oxrsys/protocol/Foveation.h>
#include <oxrsys/protocol/Protocol.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace oxrsys::gaze_foveation
{

struct GazeCenterOffset
{
    float x = 0.0f;
    float y = 0.0f;
    bool valid = false;
};

// Map an eye-gaze direction (head space, -Z forward) to the foveation centre shift the encoder
// expects, in [-1, 1] per axis.
//
// The warp applies one un-mirrored shift to both eye halves, so the best shared centre is the
// average of the two eyes' fixation points. With mirrored per-eye FOVs the horizontal asymmetry
// terms cancel, leaving tan(gaze) over the half-span; the vertical axis is the same for both
// eyes and keeps its recentring term. The shader places the sharp region's centre at source UV
// 0.5 + shift * (1 - centerSize) / 2, hence the 1 / (1 - centerSize) steering scale; without it
// the foveal region only travels a fraction of the way toward the fixation point.
//
// SIGN CONVENTION: +x moves the foveal region right; y is negated because gaze is Y-up while
// the encoder's UV space is Y-down. This is the one part of the path that cannot be verified
// headlessly -- confirm visually that the sharp region tracks the eye rather than mirroring it
// before trusting it on hardware.
inline GazeCenterOffset ComputeGazeCenterOffset(const oxr::protocol::TrackingPacket& packet,
                                                float centerSizeX,
                                                float centerSizeY)
{
    if ((packet.trackingFlags & oxr::protocol::TRACKING_FLAG_EYE_GAZE_ACTIVE) == 0)
    {
        return {};
    }

    // Network input: a NaN survives every comparison and clamp below, and once inside the
    // smoothing filter it never leaves. Reject the packet instead.
    for (const float component : packet.gazeDirection)
    {
        if (!std::isfinite(component))
        {
            return {};
        }
    }
    for (const float angle : packet.eyeFov)
    {
        if (!std::isfinite(angle))
        {
            return {};
        }
    }

    const float forward = -packet.gazeDirection[2];
    if (forward <= 0.05f)
    {
        // Gaze at or behind the view plane: no meaningful centre, keep the previous one.
        return {};
    }

    const float tanGazeX = packet.gazeDirection[0] / forward;
    const float tanGazeY = packet.gazeDirection[1] / forward;

    // eyeFov is the left eye's OpenXR-signed (left, right, up, down) angles; all-zero pairs mean
    // the client reported none, so fall back to a symmetric 45 degree half-angle.
    float tanLeft = -1.0f;
    float tanRight = 1.0f;
    if (packet.eyeFov[0] != 0.0f || packet.eyeFov[1] != 0.0f)
    {
        tanLeft = std::tan(packet.eyeFov[0]);
        tanRight = std::tan(packet.eyeFov[1]);
    }
    float tanUp = 1.0f;
    float tanDown = -1.0f;
    if (packet.eyeFov[2] != 0.0f || packet.eyeFov[3] != 0.0f)
    {
        tanUp = std::tan(packet.eyeFov[2]);
        tanDown = std::tan(packet.eyeFov[3]);
    }

    const float halfSpanX = (tanRight - tanLeft) * 0.5f;
    const float halfSpanY = (tanUp - tanDown) * 0.5f;
    if (halfSpanX <= 0.0001f || halfSpanY <= 0.0001f)
    {
        return {};
    }

    const float steerScaleX = 1.0f - std::clamp(centerSizeX, 0.0f, 1.0f);
    const float steerScaleY = 1.0f - std::clamp(centerSizeY, 0.0f, 1.0f);
    if (steerScaleX <= 0.0f || steerScaleY <= 0.0f)
    {
        // The foveal region covers the whole frame; there is nowhere to steer.
        return {};
    }

    GazeCenterOffset offset;
    offset.x = std::clamp(tanGazeX / halfSpanX / steerScaleX, -1.0f, 1.0f);
    const float centerTanY = (tanUp + tanDown) * 0.5f;
    offset.y = std::clamp(-((tanGazeY - centerTanY) / halfSpanY) / steerScaleY, -1.0f, 1.0f);
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y))
    {
        return {};
    }
    offset.valid = true;
    return offset;
}

// Low-pass over the gaze centre. Follows fixation with a light lag so jitter does not shimmer
// the foveal boundary, and decays back to the static centre while gaze is inactive (blink,
// tracking dropout, client stopped reporting) instead of freezing at the last fixation.
//
// Atomics because Update runs on the encode thread while Reset runs from the disconnect path; a
// lost update in that race is harmless, a torn float write would not be.
class GazeCenterFilter
{
public:
    struct QuantizedCenter
    {
        int8_t x = 0;
        int8_t y = 0;
    };

    QuantizedCenter Update(const GazeCenterOffset& offset)
    {
        // Per-frame constant, so the response time scales with refresh rate (~2 frames to 60%).
        // Chosen analytically, not tuned on eye-tracking hardware.
        constexpr float kSmoothing = 0.35f;
        const float targetX = offset.valid ? offset.x : 0.0f;
        const float targetY = offset.valid ? offset.y : 0.0f;
        const float x =
            x_.load(std::memory_order_relaxed) * (1.0f - kSmoothing) + targetX * kSmoothing;
        const float y =
            y_.load(std::memory_order_relaxed) * (1.0f - kSmoothing) + targetY * kSmoothing;
        x_.store(x, std::memory_order_relaxed);
        y_.store(y, std::memory_order_relaxed);
        return {oxr::protocol::QuantizeCenterShift(x), oxr::protocol::QuantizeCenterShift(y)};
    }

    void Reset()
    {
        x_.store(0.0f, std::memory_order_relaxed);
        y_.store(0.0f, std::memory_order_relaxed);
    }

private:
    std::atomic<float> x_{0.0f};
    std::atomic<float> y_{0.0f};
};

} // namespace oxrsys::gaze_foveation
