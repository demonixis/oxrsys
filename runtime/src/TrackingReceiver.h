// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "RuntimeSockets.h"

#include <oxrsys/protocol/Protocol.h>

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

    // Get the latest tracking data (thread-safe)
    bool GetLatestPose(oxr::protocol::TrackingPacket& outPacket) const;
    bool GetPredictedPose(oxr::protocol::TrackingPacket& outPacket) const;

    // Inject a tracking packet from TCP (USB mode) — same effect as receiving via UDP
    void InjectPacket(const uint8_t* data, size_t size);

    void SetPredictionHorizonMs(float predictionHorizonMs);
    float GetPredictionHorizonMs() const { return predictionHorizonMs_.load(); }

    // Check if we're receiving tracking data
    bool IsReceiving() const { return hasData_.load(); }
    bool IsRunning() const { return running_.load(); }

    // Stats
    uint64_t GetPacketCount() const { return packetCount_.load(); }

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

    mutable std::mutex poseMutex_;
    oxr::protocol::TrackingPacket latestPacket_ = {};
    std::deque<HistorySample> history_;
    std::atomic<float> predictionHorizonMs_{0.0f};
    mutable std::atomic<int64_t> lastPredictionDiagnosticNs_{0};

    static constexpr size_t MaxHistorySamples = 8;
};
