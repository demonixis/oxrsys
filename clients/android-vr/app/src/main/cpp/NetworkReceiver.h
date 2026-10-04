// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <oxrsys/protocol/Protocol.h>

#include <oxrsys/streaming/VideoFrameAssembler.h>

namespace oxr
{

/**
 * Receiver for video packets from the macOS server via WiFi/UDP.
 *
 * Runs a background thread that:
 * 1. Listens for server discovery broadcasts
 * 2. Receives encoded video packets and reassembles NAL units
 * 3. Recovers lost packets using FEC parity when possible
 */
class NetworkReceiver
{
public:
    using OnNalUnitCallback = std::function<void(const uint8_t* data, size_t size,
                                                 int64_t timestampNs, int64_t receiveTimeNs,
                                                 uint8_t flags, uint8_t codec)>;
    using OnConnectionLostCallback = std::function<void(const char* reason)>;
    // serverIp is the IP address of the broadcasting server (from recvfrom)
    using OnServerFoundCallback = std::function<void(const protocol::ServerAnnounce& server,
                                                      const char* serverIp)>;

    NetworkReceiver() = default;
    ~NetworkReceiver();

    // Non-copyable
    NetworkReceiver(const NetworkReceiver&) = delete;
    NetworkReceiver& operator=(const NetworkReceiver&) = delete;

    bool StartDiscovery(OnServerFoundCallback callback);
    void StopDiscovery();
    bool StartReceiving(const char* serverIp, uint16_t videoPort, OnNalUnitCallback callback,
                        OnConnectionLostCallback connectionLostCallback);
    bool StartReceivingTcp(uint16_t videoPort, OnNalUnitCallback callback,
                           OnConnectionLostCallback connectionLostCallback);

    // Set the control socket for sending NACKs (owned by XrApp, not NetworkReceiver)
    void SetControlSocket(int socket, const char* serverIp);

    // Selects the FEC group layout. Must match the server, which decides from
    // CLIENT_CAPABILITY_FEC_INTERLEAVED in our ClientConnect. Atomic because it is set from
    // the connection thread and read on the video receive thread.
    void SetFecInterleaved(bool interleaved) { fecInterleaved_.store(interleaved); }

    void Stop();

    bool IsReceiving() const { return receiving_.load(); }
    bool IsDiscovering() const { return discovering_.load(); }
    uint32_t GetPacketsReceived() const { return packetsReceived_.load(); }
    uint32_t GetFramesDelivered() const { return framesDelivered_.load(); }
    uint32_t GetFramesDropped() const { return framesDropped_.load(); }
    uint32_t GetFecRecoveries() const { return fecRecoveries_.load(); }

    // Render pose from the server (set when VIDEO_FLAG_RENDER_POSE packets arrive)
    struct RenderPose
    {
        uint32_t frameIndex = 0;
        int64_t presentationTimeNs = 0;
        int64_t presentationTimeUs = 0;
        float position[3] = {};
        float orientation[4] = {0, 0, 0, 1};
        bool valid = false;
    };
    RenderPose GetLatestRenderPose() const;
    bool TakeRenderPoseForPresentationTimeUs(int64_t presentationTimeUs,
                                             RenderPose* outPose);

    int64_t GetLastCompletedFrameReceiveTimeNs() const
    {
        return lastCompletedFrameReceiveTimeNs_.load();
    }

private:
    void DiscoveryThread(OnServerFoundCallback callback);
    void ReceiveThread(OnNalUnitCallback callback);
    void ReceiveTcpThread(OnNalUnitCallback callback);
    void WireAssemblerCallbacks();
    void SendNack(uint32_t frameIndex, uint32_t totalPackets, const uint8_t* packetReceived);
    void StoreRenderPose(const protocol::VideoPacketHeader& header,
                         const uint8_t* payload, size_t payloadSize);
    void StoreRenderPose(const protocol::TcpRenderPose& pose);

    // Set from SERVER_FEATURE_FEC_INTERLEAVED in the announce; selects the FEC group layout.
    std::atomic<bool> fecInterleaved_{false};
    int videoSocket_ = -1;
    int discoverySocket_ = -1;

    std::thread discoveryThread_;
    std::thread receiveThread_;
    std::atomic<bool> receiving_{false};
    std::atomic<bool> discovering_{false};

    // Frame reassembly (UDP mode only); used exclusively on the receive thread.
    streaming::VideoFrameAssembler assembler_;
    OnNalUnitCallback nalCallback_;
    OnConnectionLostCallback connectionLostCallback_;

    // NACK support
    int controlSocket_ = -1;
    std::string serverIp_;

    // Stats for logging
    std::atomic<uint32_t> packetsReceived_{0};
    std::atomic<uint32_t> framesDelivered_{0};
    std::atomic<uint32_t> framesDropped_{0};
    std::atomic<uint32_t> fecRecoveries_{0};
    std::atomic<uint32_t> nacksSent_{0};
    std::atomic<int64_t> lastCompletedFrameReceiveTimeNs_{0};

    // Latest render pose from server
    mutable std::mutex renderPoseMutex_;
    RenderPose latestRenderPose_;
    std::deque<RenderPose> renderPoses_;
    static constexpr size_t MaxRenderPoses = 128;
};

} // namespace oxr
