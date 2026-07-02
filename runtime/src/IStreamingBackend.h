// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>

#include "GraphicsTypes.h"

class TrackingReceiver;

/**
 * Interface between Session and a streaming backend implementation.
 *
 * Implementations:
 *  - StreamingServer: the oxrsys wire protocol (own Quest/Apple/Qt clients)
 *  - AlvrStreamingBackend: ALVR's embedded server_core (stock ALVR clients)
 *
 * Selected via `protocol = "oxrsys" | "alvr"` in oxrsys-runtime.toml.
 */
class IStreamingBackend
{
public:
    virtual ~IStreamingBackend() = default;

    virtual bool Start(uint32_t renderWidth, uint32_t renderHeight, uint32_t refreshRateHz) = 0;
    virtual void Stop() = 0;

    // Queue a rendered frame for asynchronous latest-frame-only encoding.
    virtual void SendFrame(FrameSource frameSource) = 0;

    virtual void SetGraphicsContext(const GraphicsContext& graphicsContext) = 0;

    virtual bool IsClientConnected() const = 0;
    virtual uint32_t GetTargetRefreshRateHz() const = 0;
    virtual std::string GetClientName() const = 0;

    // Pose source consumed by InputManager (InjectPacket seam).
    virtual TrackingReceiver* GetTrackingReceiver() = 0;

    // Optional capabilities; backends without support keep the defaults.
    virtual void ApplyHaptics(int /*hand*/, float /*amplitude*/, float /*durationSeconds*/,
                              float /*frequencyHz*/)
    {
    }

    // True if the backend supplies its own frame pacing (nanoseconds until the
    // next client vsync). Session::WaitFrame falls back to its fixed grid
    // otherwise.
    virtual bool GetFramePacing(int64_t& /*outSleepNs*/) { return false; }
};
