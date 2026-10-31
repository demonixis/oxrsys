// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "RuntimeSockets.h"

#include <Protocol.h>

/**
 * Receives 6DOF tracking data from the headset client via UDP.
 *
 * Runs a background thread that listens for TrackingPackets.
 * The latest pose data is stored and can be read by InputManager
 * to feed into the OpenXR runtime.
 */
class TrackingReceiver
{
public:
    TrackingReceiver() = default;
    ~TrackingReceiver();

    // Non-copyable
    TrackingReceiver(const TrackingReceiver&) = delete;
    TrackingReceiver& operator=(const TrackingReceiver&) = delete;

    bool Start();
    void Stop();
    void Reset();

    // Get the latest tracking data (thread-safe)
    bool GetLatestPose(oxr::protocol::TrackingPacket& outPacket) const;
    bool GetFreshPose(oxr::protocol::TrackingPacket& outPacket) const;
    bool GetPredictedPose(oxr::protocol::TrackingPacket& outPacket) const;

    // Inject a tracking packet from TCP (USB mode) — same effect as receiving via UDP
    void InjectPacket(const uint8_t* data, size_t size);

    // A UDP tracking datagram. It is stored only when its source is the connected client's
    // address (see SetClientAddress); any other sender is ignored.
    void ReceivePacket(const uint8_t* data, size_t size, const sockaddr_in& source);

    // The admitted WiFi client's IPv4 address in network byte order, or 0 when no WiFi
    // client is connected (UDP tracking is then ignored). The port is not compared.
    void SetClientAddress(uint32_t address) { clientAddress_.store(address); }

    void SetPredictionHorizonMs(float predictionHorizonMs);
    float GetPredictionHorizonMs() const { return predictionHorizonMs_.load(); }

    // Check if we're receiving tracking data
    bool IsReceiving() const { return hasData_.load(); }
    bool IsRunning() const { return running_.load(); }

    // Stats
    uint64_t GetPacketCount() const { return packetCount_.load(); }

    // Forget the previous client's samples. The reorder guard in StorePacket compares client
    // timestamps, and clients do not share a clock: a headset whose XrTime is seconds of uptime
    // would have every packet dropped as "reordered" behind a simulator that ran for days.
    void ResetHistory();
    uint64_t GetReorderedDropCount() const { return reorderedDropCount_.load(); }

private:
    struct HistorySample
    {
        oxr::protocol::TrackingPacket packet = {};
        int64_t receiveTimeNs = 0;
    };

    void ReceiveThread();
    void StorePacket(const oxr::protocol::TrackingPacket& packet, int64_t receiveTimeNs);

    oxrsys::runtime_socket::SocketHandle socket_ = oxrsys::runtime_socket::InvalidSocket;
    std::thread receiveThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> hasData_{false};
    std::atomic<uint64_t> packetCount_{0};
    std::atomic<uint64_t> reorderedDropCount_{0};
    std::atomic<uint32_t> clientAddress_{0};

    mutable std::mutex poseMutex_;
    oxr::protocol::TrackingPacket latestPacket_ = {};
    std::deque<HistorySample> history_;
    std::atomic<float> predictionHorizonMs_{0.0f};
    mutable std::atomic<int64_t> lastPredictionDiagnosticNs_{0};

    static constexpr size_t MaxHistorySamples = 8;
};
