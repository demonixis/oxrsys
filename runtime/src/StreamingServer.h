// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RuntimeSockets.h"
#include "GraphicsTypes.h"
#include "StreamingFrameQueue.h"

// Shared protocol definitions
#include <oxrsys/protocol/Protocol.h>

class VideoEncoder;
class TrackingReceiver;

/**
 * Streaming server that broadcasts its presence and streams VR content
 * to a connected headset client (Quest, Pico, etc.).
 *
 * Lifecycle:
 * 1. Start() → begins broadcasting ServerAnnounce on UDP
 * 2. Client responds with ClientConnect → stops broadcasting, marks connected
 * 3. SendFrame() called each EndFrame → encodes and streams video
 * 4. TrackingReceiver feeds pose data into InputManager
 * 5. Stop() → cleans up
 */
class StreamingServer
{
public:
    enum class State
    {
        Stopped,
        Broadcasting,
        Connected,
    };

    StreamingServer();
    ~StreamingServer();

    // Non-copyable
    StreamingServer(const StreamingServer&) = delete;
    StreamingServer& operator=(const StreamingServer&) = delete;

    // Start broadcasting and listening for clients
    bool Start(uint32_t renderWidth, uint32_t renderHeight, uint32_t refreshRateHz);
    void Stop();

    // Queue a rendered frame for asynchronous latest-frame-only encoding.
    // The source owns backend graphics resources until the frame is encoded,
    // dropped, or replaced by a newer pending frame.
    void SendFrame(FrameSource frameSource);

    // Set the platform graphics device for VideoEncoder initialization.
    void SetGraphicsContext(const GraphicsContext& graphicsContext) { graphicsContext_ = graphicsContext; }
    void SetGraphicsDevice(void* graphicsDevice) { SetGraphicsContext(GraphicsContext::Metal(graphicsDevice)); }
    void SetMetalDevice(void* metalDevice) { SetGraphicsDevice(metalDevice); }

    // Check if a client is connected
    bool IsClientConnected() const { return state_.load() == State::Connected; }
    State GetState() const { return state_.load(); }
    uint32_t GetTargetRefreshRateHz() const { return targetRefreshRateHz_.load(); }

    // Get the connected client info
    std::string GetClientName() const;

    // Access to tracking receiver (for InputManager integration)
    TrackingReceiver* GetTrackingReceiver() { return trackingReceiver_.get(); }

private:
    using SocketHandle = oxrsys::runtime_socket::SocketHandle;

    struct CachedPacket
    {
        std::vector<uint8_t> data;  // Full packet (header + payload)
    };

    struct CachedFrame
    {
        uint32_t frameIndex = 0;
        std::vector<CachedPacket> packets;
    };

    struct PacketDispatchState
    {
        std::mutex mutex;
        std::mutex sendMutex;
        std::string clientIp;
        SocketHandle videoSocket = oxrsys::runtime_socket::InvalidSocket;
        bool videoUsesTcp = false;
        bool acceptingPackets = false;

        // Ring buffer of recently sent frames for NACK retransmission
        static constexpr size_t MaxCachedFrames = 5;
        std::array<CachedFrame, MaxCachedFrames> cachedFrames;
        size_t cacheWriteIndex = 0;
    };

    void BroadcastThread();
    void ControlThread();
    void EncodeThread();
    void TcpControlThread();
    void TcpVideoThread();
    void TcpTrackingThread();
    std::string GetLocalIpAddress() const;
    oxr::protocol::ServerAnnounce BuildServerAnnounce() const;
    void ReleaseStreamingFrame(StreamingFrame& frame);
    void HandleClientConnect(const oxr::protocol::ClientConnect& clientConnect,
                             const sockaddr_in& clientAddr);
    void HandleUsbClientConnect(const oxr::protocol::ClientConnect& clientConnect);
    void HandleClientDisconnect();
    void HandleLatencyReport(const oxr::protocol::LatencyReport& report);
    void HandleKeyframeRequest(const oxr::protocol::RequestKeyframe& request);
    void HandleNackRequest(const oxr::protocol::NackRequest& request);
    void HandleControlPayload(const uint8_t* data, size_t size);
    void UpdatePredictionHorizon();
    bool StartUsbTcpListeners();
    void SendUsbDisconnectBestEffort();
    void StopUsbTcpSockets();

    std::atomic<State> state_{State::Stopped};

    // Server config
    uint32_t renderWidth_ = 0;
    uint32_t renderHeight_ = 0;
    uint32_t scaledWidth_ = 0;      // After resolution_scale (per eye)
    uint32_t scaledHeight_ = 0;     // After resolution_scale
    uint32_t refreshRateHz_ = 90;
    std::atomic<uint32_t> targetRefreshRateHz_{90};
    std::atomic<float> clientPipelineLatencyMs_{18.0f};
    std::atomic<float> serverPipelineLatencyMs_{10.0f};
    std::atomic<float> clientReceiveToSubmitMs_{0.0f};
    std::atomic<float> clientDecodeLatencyMs_{0.0f};
    std::atomic<float> clientCompositorLatencyMs_{0.0f};

    // Adaptive bitrate state
    std::atomic<uint32_t> configMaxBitrateMbps_{50};
    std::atomic<uint32_t> currentBitrateMbps_{50};
    int64_t lastBitrateIncreaseTimeNs_ = 0;
    uint32_t lastKeyframeRequestCountForAbr_ = 0;

    // Sockets
    SocketHandle broadcastSocket_ = oxrsys::runtime_socket::InvalidSocket;
    SocketHandle controlSocket_ = oxrsys::runtime_socket::InvalidSocket;
    SocketHandle videoSocket_ = oxrsys::runtime_socket::InvalidSocket;
    SocketHandle tcpControlListenSocket_ = oxrsys::runtime_socket::InvalidSocket;
    SocketHandle tcpVideoListenSocket_ = oxrsys::runtime_socket::InvalidSocket;
    SocketHandle tcpTrackingListenSocket_ = oxrsys::runtime_socket::InvalidSocket;
    SocketHandle tcpControlClientSocket_ = oxrsys::runtime_socket::InvalidSocket;
    SocketHandle tcpVideoClientSocket_ = oxrsys::runtime_socket::InvalidSocket;
    SocketHandle tcpTrackingClientSocket_ = oxrsys::runtime_socket::InvalidSocket;
    std::mutex tcpSocketMutex_;
    std::mutex disconnectMutex_;
    bool wifiEnabled_ = true;
    bool usbAdbEnabled_ = true;
    std::atomic_bool clientUsesUsbAdb_{false};

    // Client info
    std::string clientIp_;
    uint16_t clientPort_ = 0;
    std::string clientName_;
    mutable std::mutex clientMutex_;

    // Threads
    std::thread broadcastThread_;
    std::thread controlThread_;
    std::thread encodeThread_;
    std::thread tcpControlThread_;
    std::thread tcpVideoThread_;
    std::thread tcpTrackingThread_;
    std::atomic<bool> running_{false};
    StreamingFrameQueue frameQueue_;
    std::shared_ptr<PacketDispatchState> packetDispatchState_;
    std::atomic<uint32_t> pendingFrameDepthMax_{0};
    std::atomic<uint32_t> replacedFrameCount_{0};
    std::atomic<uint32_t> requestKeyframeCount_{0};
    std::mutex encoderMutex_;

    static void SendNalUnit(const std::shared_ptr<PacketDispatchState>& dispatchState,
                            uint32_t frameIndex, const uint8_t* data, size_t size,
                            bool isKeyframe, int64_t timestampNs);
    static bool SendTcpRecord(SocketHandle socket, oxr::protocol::TcpRecordType type,
                              const void* payload, size_t payloadSize);
    static void MarkTcpVideoSendFailed(const std::shared_ptr<PacketDispatchState>& dispatchState,
                                       SocketHandle socket);

    // Sub-components
    std::shared_ptr<VideoEncoder> encoder_;
    std::unique_ptr<TrackingReceiver> trackingReceiver_;
    GraphicsContext graphicsContext_ = {};

    // Frame sending state
    uint32_t frameIndex_ = 0;
};
