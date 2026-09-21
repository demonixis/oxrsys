// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstdint>

#include <oxrsys/protocol/Protocol.h>

namespace oxr
{

/**
 * Sends headset and controller tracking data back to the macOS server via UDP.
 *
 * Called from the XR frame loop after xrLocateViews/xrLocateSpace.
 * Sends at the OpenXR frame rate (typically 72-120 Hz).
 */
class TrackingSender
{
public:
    TrackingSender() = default;
    ~TrackingSender();

    // Non-copyable
    TrackingSender(const TrackingSender&) = delete;
    TrackingSender& operator=(const TrackingSender&) = delete;

    // WiFi/UDP mode
    bool Connect(const char* serverIp, uint16_t trackingPort);

    // USB/TCP mode — connect to localhost via adb reverse
    bool ConnectTcp(uint16_t trackingPort);

    void Disconnect();

    bool Send(const protocol::TrackingPacket& packet);

    bool IsConnected() const { return socket_ >= 0; }
    bool IsUsbMode() const { return usbMode_; }

private:
    int socket_ = -1;
    bool usbMode_ = false;
};

} // namespace oxr
