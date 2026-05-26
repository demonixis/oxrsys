// SPDX-License-Identifier: MPL-2.0

#include "StreamingServer.h"
#include "Config.h"
#include "RuntimeStatus.h"
#include "Swapchain.h"
#include "TrackingReceiver.h"
#include "VideoEncoder.h"
#include "../../clients/common/src/FecCodec.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <numeric>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

namespace
{

using Clock = std::chrono::steady_clock;

int64_t SteadyClockNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

bool SendAll(int socket, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t sentTotal = 0;
    while (sentTotal < size)
    {
        ssize_t sent = send(socket, bytes + sentTotal, size - sentTotal, 0);
        if (sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (sent == 0)
        {
            return false;
        }
        sentTotal += static_cast<size_t>(sent);
    }
    return true;
}

bool ReadAll(int socket, void* data, size_t size)
{
    auto* bytes = static_cast<uint8_t*>(data);
    size_t receivedTotal = 0;
    while (receivedTotal < size)
    {
        ssize_t received = recv(socket, bytes + receivedTotal, size - receivedTotal, 0);
        if (received < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (received == 0)
        {
            return false;
        }
        receivedTotal += static_cast<size_t>(received);
    }
    return true;
}

bool ReadTcpRecord(int socket, oxr::protocol::TcpRecordHeader& header,
                   std::vector<uint8_t>& payload)
{
    if (!ReadAll(socket, &header, sizeof(header)))
    {
        return false;
    }
    if (header.magic != oxr::protocol::TCP_RECORD_MAGIC ||
        header.version != oxr::protocol::TCP_RECORD_VERSION ||
        header.payloadSize > oxr::protocol::TCP_MAX_RECORD_PAYLOAD)
    {
        return false;
    }

    payload.clear();
    payload.resize(header.payloadSize);
    if (header.payloadSize == 0)
    {
        return true;
    }
    return ReadAll(socket, payload.data(), payload.size());
}

void ConfigureTcpSocket(int socket)
{
    int yes = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}

void CloseTcpSocket(int& socket)
{
    if (socket >= 0)
    {
        shutdown(socket, SHUT_RDWR);
        close(socket);
        socket = -1;
    }
}

int CreateLoopbackListener(uint16_t port)
{
    int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket < 0)
    {
        return -1;
    }

    int yes = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listenSocket, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(listenSocket);
        return -1;
    }

    if (listen(listenSocket, 1) < 0)
    {
        close(listenSocket);
        return -1;
    }

    return listenSocket;
}

int AcceptWithTimeout(int listenSocket)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenSocket, &readSet);
    timeval timeout = {1, 0};

    int ready = select(listenSocket + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0 || !FD_ISSET(listenSocket, &readSet))
    {
        return -1;
    }

    int clientSocket = accept(listenSocket, nullptr, nullptr);
    if (clientSocket >= 0)
    {
        ConfigureTcpSocket(clientSocket);
    }
    return clientSocket;
}

struct MetricSummary
{
    double average = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    size_t count = 0;
};

MetricSummary Summarize(std::vector<double>& values)
{
    MetricSummary summary = {};
    if (values.empty())
    {
        return summary;
    }

    std::sort(values.begin(), values.end());
    summary.count = values.size();
    summary.average = std::accumulate(values.begin(), values.end(), 0.0) / summary.count;

    auto percentile = [&values](double p) {
        size_t index = static_cast<size_t>(std::clamp(p, 0.0, 1.0) * (values.size() - 1));
        return values[index];
    };

    summary.p50 = percentile(0.50);
    summary.p95 = percentile(0.95);
    return summary;
}

class TelemetryWindow
{
public:
    void Add(double sample)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.push_back(sample);
    }

    MetricSummary Consume()
    {
        std::vector<double> samples;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            samples.swap(samples_);
        }
        return Summarize(samples);
    }

private:
    std::mutex mutex_;
    std::vector<double> samples_;
};

struct EncodeTelemetry
{
    TelemetryWindow queueWaitMs;
    TelemetryWindow gpuCopyMs;
    TelemetryWindow encodeSubmitMs;
    TelemetryWindow callbackLatencyMs;
    TelemetryWindow totalPipelineMs;
    std::mutex logMutex;
    Clock::time_point lastLogTime = Clock::now();
};

} // namespace

StreamingServer::StreamingServer()
    : packetDispatchState_(std::make_shared<PacketDispatchState>())
{
}

StreamingServer::~StreamingServer()
{
    Stop();
}

bool StreamingServer::Start(uint32_t renderWidth, uint32_t renderHeight, uint32_t refreshRateHz)
{
    if (running_.load())
    {
        return false;
    }

    renderWidth_ = renderWidth;
    renderHeight_ = renderHeight;
    refreshRateHz_ = refreshRateHz;
    targetRefreshRateHz_.store(refreshRateHz);

    const ConfigValues config = Config::Get().GetValues();
    wifiEnabled_ = config.streamingTransport != "usb_adb";
    usbAdbEnabled_ = config.streamingTransport != "wifi";
    float scale = config.resolutionScale;
    scaledWidth_ = static_cast<uint32_t>(renderWidth * scale);
    scaledHeight_ = static_cast<uint32_t>(renderHeight * scale);
    scaledWidth_ = (scaledWidth_ + 15) & ~15u;
    scaledHeight_ = (scaledHeight_ + 15) & ~15u;

    spdlog::info("StreamingServer: Resolution scaling {:.0f}%: {}x{} -> {}x{} per eye",
                  scale * 100.0f, renderWidth, renderHeight, scaledWidth_, scaledHeight_);

    broadcastSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (broadcastSocket_ < 0)
    {
        spdlog::error("StreamingServer: Failed to create broadcast socket");
        return false;
    }

    int opt = 1;
    setsockopt(broadcastSocket_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    setsockopt(broadcastSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    controlSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (controlSocket_ < 0)
    {
        spdlog::error("StreamingServer: Failed to create control socket");
        close(broadcastSocket_);
        broadcastSocket_ = -1;
        return false;
    }

    setsockopt(controlSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in controlAddr = {};
    controlAddr.sin_family = AF_INET;
    controlAddr.sin_port = htons(oxr::protocol::CONTROL_PORT);
    controlAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(controlSocket_, (sockaddr*)&controlAddr, sizeof(controlAddr)) < 0)
    {
        spdlog::error("StreamingServer: Failed to bind control socket on port {}",
                       oxr::protocol::CONTROL_PORT);
        close(broadcastSocket_);
        close(controlSocket_);
        broadcastSocket_ = -1;
        controlSocket_ = -1;
        return false;
    }

    videoSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (videoSocket_ < 0)
    {
        spdlog::error("StreamingServer: Failed to create video socket");
        close(broadcastSocket_);
        close(controlSocket_);
        broadcastSocket_ = -1;
        controlSocket_ = -1;
        return false;
    }

    int bufferSize = 4 * 1024 * 1024;
    setsockopt(videoSocket_, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->videoSocket = videoSocket_;
        packetDispatchState_->acceptingPackets = true;
    }

    trackingReceiver_ = std::make_unique<TrackingReceiver>();
    if (!trackingReceiver_->Start())
    {
        spdlog::error("StreamingServer: Failed to start tracking receiver");
        close(broadcastSocket_);
        close(controlSocket_);
        close(videoSocket_);
        broadcastSocket_ = -1;
        controlSocket_ = -1;
        videoSocket_ = -1;
        trackingReceiver_.reset();
        return false;
    }

    running_.store(true);
    state_.store(State::Broadcasting);
    RuntimeStatus::SetIdle();

    if (usbAdbEnabled_ && !StartUsbTcpListeners())
    {
        if (!wifiEnabled_)
        {
            spdlog::error("StreamingServer: Failed to start required USB ADB TCP listeners");
            running_.store(false);
            close(broadcastSocket_);
            close(controlSocket_);
            close(videoSocket_);
            broadcastSocket_ = -1;
            controlSocket_ = -1;
            videoSocket_ = -1;
            trackingReceiver_->Stop();
            trackingReceiver_.reset();
            return false;
        }
        spdlog::warn("StreamingServer: USB ADB TCP listeners unavailable; continuing with WiFi");
        usbAdbEnabled_ = false;
    }

    if (wifiEnabled_)
    {
        broadcastThread_ = std::thread(&StreamingServer::BroadcastThread, this);
    }
    controlThread_ = std::thread(&StreamingServer::ControlThread, this);
    encodeThread_ = std::thread(&StreamingServer::EncodeThread, this);

    std::string ip = GetLocalIpAddress();
    spdlog::info("StreamingServer: Started transport={} wifi={} usb_adb={} on {} ({}x{} @ {}Hz)",
                  config.streamingTransport, wifiEnabled_, usbAdbEnabled_,
                  ip, renderWidth_, renderHeight_, refreshRateHz_);
    return true;
}

void StreamingServer::Stop()
{
    running_.store(false);
    state_.store(State::Stopped);
    frameReadyCv_.notify_all();
    RuntimeStatus::SetIdle();
    SendUsbDisconnectBestEffort();
    StopUsbTcpSockets();

    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->acceptingPackets = false;
        packetDispatchState_->clientIp.clear();
        packetDispatchState_->videoSocket = -1;
        packetDispatchState_->videoUsesTcp = false;
    }
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        clientIp_.clear();
        clientName_.clear();
        clientPort_ = 0;
        clientUsesUsbAdb_ = false;
    }

    if (broadcastSocket_ >= 0)
    {
        close(broadcastSocket_);
        broadcastSocket_ = -1;
    }
    if (controlSocket_ >= 0)
    {
        close(controlSocket_);
        controlSocket_ = -1;
    }
    if (videoSocket_ >= 0)
    {
        close(videoSocket_);
        videoSocket_ = -1;
    }

    if (broadcastThread_.joinable())
    {
        broadcastThread_.join();
    }
    if (controlThread_.joinable())
    {
        controlThread_.join();
    }
    if (encodeThread_.joinable())
    {
        encodeThread_.join();
    }
    if (tcpControlThread_.joinable())
    {
        tcpControlThread_.join();
    }
    if (tcpVideoThread_.joinable())
    {
        tcpVideoThread_.join();
    }
    if (tcpTrackingThread_.joinable())
    {
        tcpTrackingThread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        ReleasePendingFrame(pendingFrame_);
    }

    if (trackingReceiver_ != nullptr)
    {
        trackingReceiver_->Stop();
        trackingReceiver_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        encoder_.reset();
    }

    spdlog::info("StreamingServer: Stopped");
}

void StreamingServer::BroadcastThread()
{
    oxr::protocol::ServerAnnounce announce = BuildServerAnnounce();

    sockaddr_in broadcastAddr = {};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(oxr::protocol::DISCOVERY_PORT);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    while (running_.load() && state_.load() == State::Broadcasting)
    {
        sendto(broadcastSocket_, &announce, sizeof(announce), 0,
               (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

        for (int i = 0; i < 10 && running_.load() && state_.load() == State::Broadcasting; i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    spdlog::info("StreamingServer: Broadcast thread ended");
}

oxr::protocol::ServerAnnounce StreamingServer::BuildServerAnnounce() const
{
    oxr::protocol::ServerAnnounce announce = {};
    announce.type = oxr::protocol::MessageType::ServerAnnounce;
    announce.versionMajor = 1;
    announce.versionMinor = 0;
    announce.videoPort = oxr::protocol::VIDEO_PORT;
    announce.trackingPort = oxr::protocol::TRACKING_PORT;
    announce.renderWidth = renderWidth_ * 2;
    announce.renderHeight = renderHeight_;
    announce.refreshRateHz = refreshRateHz_;
    announce.encodedWidth = scaledWidth_ * 2;
    announce.encodedHeight = scaledHeight_;
    strncpy(announce.serverName, "OXRSys Runtime", sizeof(announce.serverName) - 1);
    return announce;
}

void StreamingServer::ControlThread()
{
    uint8_t buffer[512];

    while (running_.load())
    {
        timeval tv = {1, 0};
        setsockopt(controlSocket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in clientAddr = {};
        socklen_t addrLen = sizeof(clientAddr);
        ssize_t received = recvfrom(controlSocket_, buffer, sizeof(buffer), 0,
                                     (sockaddr*)&clientAddr, &addrLen);
        if (received < 1)
        {
            continue;
        }

        uint8_t type = buffer[0];
        if (type == static_cast<uint8_t>(oxr::protocol::MessageType::ClientConnect) &&
            received >= (ssize_t)sizeof(oxr::protocol::ClientConnect))
        {
            if (wifiEnabled_)
            {
                HandleClientConnect(*reinterpret_cast<oxr::protocol::ClientConnect*>(buffer), clientAddr);
            }
        }
        else if (type == static_cast<uint8_t>(oxr::protocol::MessageType::ServerDisconnect))
        {
            HandleClientDisconnect();
        }
        else
        {
            HandleControlPayload(buffer, static_cast<size_t>(received));
        }
    }

    spdlog::info("StreamingServer: Control thread ended");
}

bool StreamingServer::StartUsbTcpListeners()
{
    tcpControlListenSocket_ = CreateLoopbackListener(oxr::protocol::CONTROL_PORT);
    tcpVideoListenSocket_ = CreateLoopbackListener(oxr::protocol::VIDEO_PORT);
    tcpTrackingListenSocket_ = CreateLoopbackListener(oxr::protocol::TRACKING_PORT);
    if (tcpControlListenSocket_ < 0 || tcpVideoListenSocket_ < 0 || tcpTrackingListenSocket_ < 0)
    {
        StopUsbTcpSockets();
        return false;
    }

    tcpControlThread_ = std::thread(&StreamingServer::TcpControlThread, this);
    tcpVideoThread_ = std::thread(&StreamingServer::TcpVideoThread, this);
    tcpTrackingThread_ = std::thread(&StreamingServer::TcpTrackingThread, this);
    spdlog::info("StreamingServer: USB ADB TCP listeners active on localhost ports {}/{}/{}",
                  oxr::protocol::CONTROL_PORT,
                  oxr::protocol::VIDEO_PORT,
                  oxr::protocol::TRACKING_PORT);
    return true;
}

void StreamingServer::SendUsbDisconnectBestEffort()
{
    int controlSocket = -1;
    {
        std::lock_guard<std::mutex> lock(tcpSocketMutex_);
        controlSocket = tcpControlClientSocket_;
    }
    if (controlSocket >= 0)
    {
        SendTcpRecord(controlSocket, oxr::protocol::TcpRecordType::Disconnect, nullptr, 0);
    }
}

void StreamingServer::StopUsbTcpSockets()
{
    std::lock_guard<std::mutex> lock(tcpSocketMutex_);
    int* sockets[] = {
        &tcpControlListenSocket_,
        &tcpVideoListenSocket_,
        &tcpTrackingListenSocket_,
        &tcpControlClientSocket_,
        &tcpVideoClientSocket_,
        &tcpTrackingClientSocket_,
    };
    for (int* socketPtr : sockets)
    {
        CloseTcpSocket(*socketPtr);
    }
}

void StreamingServer::TcpControlThread()
{
    while (running_.load() && usbAdbEnabled_)
    {
        int clientSocket = AcceptWithTimeout(tcpControlListenSocket_);
        if (clientSocket < 0)
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (tcpControlClientSocket_ >= 0)
            {
                CloseTcpSocket(tcpControlClientSocket_);
            }
            tcpControlClientSocket_ = clientSocket;
        }

        oxr::protocol::ServerAnnounce announce = BuildServerAnnounce();
        if (!SendTcpRecord(clientSocket, oxr::protocol::TcpRecordType::ServerAnnounce,
                           &announce, sizeof(announce)))
        {
            {
                std::lock_guard<std::mutex> lock(tcpSocketMutex_);
                if (tcpControlClientSocket_ == clientSocket)
                {
                    tcpControlClientSocket_ = -1;
                }
            }
            CloseTcpSocket(clientSocket);
            continue;
        }

        spdlog::info("StreamingServer: USB ADB control client connected");
        while (running_.load())
        {
            oxr::protocol::TcpRecordHeader header = {};
            std::vector<uint8_t> payload;
            if (!ReadTcpRecord(clientSocket, header, payload))
            {
                break;
            }

            if (header.type == oxr::protocol::TcpRecordType::ClientConnect &&
                payload.size() >= sizeof(oxr::protocol::ClientConnect))
            {
                HandleUsbClientConnect(*reinterpret_cast<const oxr::protocol::ClientConnect*>(
                    payload.data()));
            }
            else if (header.type == oxr::protocol::TcpRecordType::Control)
            {
                HandleControlPayload(payload.data(), payload.size());
            }
            else if (header.type == oxr::protocol::TcpRecordType::Disconnect)
            {
                HandleClientDisconnect();
                break;
            }
        }

        bool shouldClose = true;
        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (tcpControlClientSocket_ == clientSocket)
            {
                tcpControlClientSocket_ = -1;
            }
            else
            {
                shouldClose = false;
            }
        }
        if (shouldClose)
        {
            CloseTcpSocket(clientSocket);
        }
        if (clientUsesUsbAdb_)
        {
            HandleClientDisconnect();
        }
        spdlog::info("StreamingServer: USB ADB control client disconnected");
    }
    spdlog::info("StreamingServer: USB ADB control thread ended");
}

void StreamingServer::TcpVideoThread()
{
    while (running_.load() && usbAdbEnabled_)
    {
        int clientSocket = AcceptWithTimeout(tcpVideoListenSocket_);
        if (clientSocket < 0)
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (tcpVideoClientSocket_ >= 0)
            {
                CloseTcpSocket(tcpVideoClientSocket_);
            }
            tcpVideoClientSocket_ = clientSocket;
        }
        {
            std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
            if (clientUsesUsbAdb_)
            {
                packetDispatchState_->videoSocket = clientSocket;
                packetDispatchState_->videoUsesTcp = true;
            }
        }

        spdlog::info("StreamingServer: USB ADB video client connected");
    }
    spdlog::info("StreamingServer: USB ADB video thread ended");
}

void StreamingServer::TcpTrackingThread()
{
    while (running_.load() && usbAdbEnabled_)
    {
        int clientSocket = AcceptWithTimeout(tcpTrackingListenSocket_);
        if (clientSocket < 0)
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (tcpTrackingClientSocket_ >= 0)
            {
                CloseTcpSocket(tcpTrackingClientSocket_);
            }
            tcpTrackingClientSocket_ = clientSocket;
        }

        spdlog::info("StreamingServer: USB ADB tracking client connected");
        while (running_.load())
        {
            oxr::protocol::TcpRecordHeader header = {};
            std::vector<uint8_t> payload;
            if (!ReadTcpRecord(clientSocket, header, payload))
            {
                break;
            }
            if (header.type == oxr::protocol::TcpRecordType::Tracking &&
                payload.size() >= sizeof(oxr::protocol::TrackingPacket) &&
                trackingReceiver_ != nullptr)
            {
                trackingReceiver_->InjectPacket(payload.data(), payload.size());
            }
        }

        bool shouldClose = true;
        {
            std::lock_guard<std::mutex> lock(tcpSocketMutex_);
            if (tcpTrackingClientSocket_ == clientSocket)
            {
                tcpTrackingClientSocket_ = -1;
            }
            else
            {
                shouldClose = false;
            }
        }
        if (shouldClose)
        {
            CloseTcpSocket(clientSocket);
        }
        spdlog::info("StreamingServer: USB ADB tracking client disconnected");
    }
    spdlog::info("StreamingServer: USB ADB tracking thread ended");
}

void StreamingServer::EncodeThread()
{
    auto telemetry = std::make_shared<EncodeTelemetry>();
    std::shared_ptr<PacketDispatchState> packetDispatchState = packetDispatchState_;

    while (running_.load())
    {
        PendingFrame frame = {};
        {
            std::unique_lock<std::mutex> lock(frameMutex_);
            frameReadyCv_.wait(lock, [this] {
                return !running_.load() || pendingFrame_.valid;
            });

            if (!running_.load() && !pendingFrame_.valid)
            {
                break;
            }

            frame = pendingFrame_;
            pendingFrame_ = {};
        }

        if (!frame.valid)
        {
            continue;
        }

        double queueWaitMs = (double)(SteadyClockNowNs() - frame.timestampNs) / 1.0e6;
        telemetry->queueWaitMs.Add(queueWaitMs);

        uint32_t currentRefreshHz = std::max(targetRefreshRateHz_.load(), 1u);
        const ConfigValues config = Config::Get().GetValues();
        uint32_t keyframeFrames = std::max(config.keyframeIntervalSec * currentRefreshHz, 1u);
        bool forceKeyframe = frame.frameIndex < 5 || (frame.frameIndex % keyframeFrames == 0);

        std::shared_ptr<VideoEncoder> encoder;
        {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            if (encoder_ != nullptr)
            {
                encoder = encoder_;
            }
        }

        if (!encoder || !encoder->IsInitialized())
        {
            ReleasePendingFrame(frame);
            continue;
        }

        if (forceKeyframe)
        {
            encoder->ForceKeyframe();
        }

        // Send render pose as a video-channel packet before the frame data.
        // The client uses this to set the correct composition layer pose for ATW.
        if (frame.hasPose)
        {
            std::string clientIp;
            int videoSocket = -1;
            bool videoUsesTcp = false;
            {
                std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
                if (packetDispatchState_->acceptingPackets)
                {
                    clientIp = packetDispatchState_->clientIp;
                    videoSocket = packetDispatchState_->videoSocket;
                    videoUsesTcp = packetDispatchState_->videoUsesTcp;
                }
            }
            if (videoUsesTcp && videoSocket >= 0)
            {
                oxr::protocol::TcpRenderPose pose = {};
                pose.frameIndex = frame.frameIndex;
                pose.presentationTimeNs = frame.timestampNs;
                memcpy(pose.position, frame.headPosition, sizeof(float) * 3);
                memcpy(pose.orientation, frame.headOrientation, sizeof(float) * 4);

                std::lock_guard<std::mutex> sendLock(packetDispatchState_->sendMutex);
                SendTcpRecord(videoSocket, oxr::protocol::TcpRecordType::RenderPose,
                              &pose, sizeof(pose));
            }
            else if (!clientIp.empty() && videoSocket >= 0)
            {
                // Payload: 7 floats (position[3] + orientation[4])
                float posePayload[7];
                memcpy(posePayload, frame.headPosition, sizeof(float) * 3);
                memcpy(posePayload + 3, frame.headOrientation, sizeof(float) * 4);

                oxr::protocol::VideoPacketHeader poseHeader = {};
                poseHeader.frameIndex = frame.frameIndex;
                poseHeader.packetIndex = 0;
                poseHeader.totalPackets = 0;  // 0 totalPackets = metadata, not data
                poseHeader.payloadSize = sizeof(posePayload);
                poseHeader.flags = oxr::protocol::VIDEO_FLAG_RENDER_POSE;
                poseHeader.codec = static_cast<uint8_t>(oxr::protocol::VideoCodec::H265);
                poseHeader.presentationTimeNs = frame.timestampNs;

                uint8_t buf[sizeof(poseHeader) + sizeof(posePayload)];
                memcpy(buf, &poseHeader, sizeof(poseHeader));
                memcpy(buf + sizeof(poseHeader), posePayload, sizeof(posePayload));

                sockaddr_in destAddr = {};
                destAddr.sin_family = AF_INET;
                destAddr.sin_port = htons(oxr::protocol::VIDEO_PORT);
                inet_pton(AF_INET, clientIp.c_str(), &destAddr.sin_addr);
                sendto(videoSocket, buf, sizeof(buf), MSG_DONTWAIT,
                       (sockaddr*)&destAddr, sizeof(destAddr));
            }
        }

        bool encoded = encoder->EncodeStereo(
            frame.leftTexture,
            frame.rightTexture,
            frame.timestampNs,
            [packetDispatchState, frameIndex = frame.frameIndex, timestampNs = frame.timestampNs](
                const uint8_t* nalData, size_t nalSize, bool isKeyframe, int64_t /*pts*/)
            {
                SendNalUnit(packetDispatchState, frameIndex, nalData, nalSize, isKeyframe, timestampNs);
            },
            [this, telemetry, queueWaitMs, encoder](const VideoEncoder::FrameMetrics& metrics)
            {
                telemetry->gpuCopyMs.Add(metrics.gpuCopyMs);
                telemetry->encodeSubmitMs.Add(metrics.encodeSubmitMs);
                telemetry->callbackLatencyMs.Add(metrics.callbackLatencyMs);
                telemetry->totalPipelineMs.Add(queueWaitMs + metrics.totalLatencyMs);

                if (!metrics.frameDropped)
                {
                    float smoothed = static_cast<float>(queueWaitMs + metrics.totalLatencyMs);
                    float previous = serverPipelineLatencyMs_.load();
                    serverPipelineLatencyMs_.store(previous * 0.85f + smoothed * 0.15f);
                    UpdatePredictionHorizon();
                }

                std::lock_guard<std::mutex> logLock(telemetry->logMutex);
                auto now = Clock::now();
                if (now - telemetry->lastLogTime >= std::chrono::seconds(1))
                {
                    MetricSummary queueSummary = telemetry->queueWaitMs.Consume();
                    MetricSummary gpuSummary = telemetry->gpuCopyMs.Consume();
                    MetricSummary submitSummary = telemetry->encodeSubmitMs.Consume();
                    MetricSummary callbackSummary = telemetry->callbackLatencyMs.Consume();
                    MetricSummary totalSummary = telemetry->totalPipelineMs.Consume();

                    const uint32_t replacedFrames = replacedFrameCount_.exchange(0);
                    const uint32_t encoderDrops = encoder->GetDroppedFrameCount();
                    const uint32_t keyframeRequests = requestKeyframeCount_.exchange(0);
                    const uint32_t pendingDepthMax = pendingFrameDepthMax_.exchange(0);

                    RuntimeStatus::StreamingStats stats = {};
                    stats.refreshRateHz = targetRefreshRateHz_.load();
                    stats.currentBitrateMbps = currentBitrateMbps_.load();
                    stats.maxBitrateMbps = configMaxBitrateMbps_.load();
                    stats.renderWidth = renderWidth_ * 2;
                    stats.renderHeight = renderHeight_;
                    stats.encodedWidth = scaledWidth_ * 2;
                    stats.encodedHeight = scaledHeight_;
                    stats.serverPipelineLatencyMs = serverPipelineLatencyMs_.load();
                    stats.clientPipelineLatencyMs = clientPipelineLatencyMs_.load();
                    stats.clientReceiveToSubmitMs = clientReceiveToSubmitMs_.load();
                    stats.clientDecodeMs = clientDecodeLatencyMs_.load();
                    stats.clientCompositorMs = clientCompositorLatencyMs_.load();
                    stats.predictionHorizonMs = trackingReceiver_ ?
                        trackingReceiver_->GetPredictionHorizonMs() : 0.0f;
                    stats.encodeQueueAverageMs = queueSummary.average;
                    stats.encodeQueueP95Ms = queueSummary.p95;
                    stats.encodeGpuAverageMs = gpuSummary.average;
                    stats.encodeGpuP95Ms = gpuSummary.p95;
                    stats.encodeSubmitAverageMs = submitSummary.average;
                    stats.encodeSubmitP95Ms = submitSummary.p95;
                    stats.encodeCallbackAverageMs = callbackSummary.average;
                    stats.encodeCallbackP95Ms = callbackSummary.p95;
                    stats.encodeTotalAverageMs = totalSummary.average;
                    stats.encodeTotalP95Ms = totalSummary.p95;
                    stats.encodedFramesTotal = encoder->GetEncodedFrameCount();
                    stats.encoderDroppedFramesTotal = encoderDrops;
                    stats.replacedFramesDelta = replacedFrames;
                    stats.keyframeRequestsDelta = keyframeRequests;
                    stats.pendingDepthMax = pendingDepthMax;
                    RuntimeStatus::SetStreamingStats(stats);

                    spdlog::info(
                        "StreamingServer: encode queue(avg/p95={:.2f}/{:.2f}ms) gpu({:.2f}/{:.2f}) "
                        "submit({:.3f}/{:.3f}) callback({:.2f}/{:.2f}) total({:.2f}/{:.2f}) "
                        "replaced={} encDrops={} keyframeReq={} depthMax={}",
                        queueSummary.average, queueSummary.p95,
                        gpuSummary.average, gpuSummary.p95,
                        submitSummary.average, submitSummary.p95,
                        callbackSummary.average, callbackSummary.p95,
                        totalSummary.average, totalSummary.p95,
                        replacedFrames,
                        encoderDrops,
                        keyframeRequests,
                        pendingDepthMax);

                    telemetry->lastLogTime = now;
                }
            });

        ReleasePendingFrame(frame);

        if (!encoded)
        {
            pendingFrameDepthMax_.store(std::max(
                pendingFrameDepthMax_.load(), encoder->GetInFlightFrameCount()));
        }

        static uint32_t logFrameCounter = 0;
        if (++logFrameCounter <= 5 || logFrameCounter % 300 == 0)
        {
            spdlog::info("StreamingServer: queued frame #{} encodeSubmitted={} queueWait={:.2f}ms inflight={}",
                          frame.frameIndex, encoded, queueWaitMs, encoder->GetInFlightFrameCount());
        }
    }

    spdlog::info("StreamingServer: Encode thread ended");
}

void StreamingServer::HandleClientConnect(const oxr::protocol::ClientConnect& clientConnect,
                                          const sockaddr_in& clientAddr)
{
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
    std::string clientName(clientConnect.deviceName,
        strnlen(clientConnect.deviceName, sizeof(clientConnect.deviceName)));

    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        clientIp_ = ipStr;
        clientPort_ = ntohs(clientAddr.sin_port);
        clientName_ = clientName;
        clientUsesUsbAdb_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->clientIp = ipStr;
        packetDispatchState_->videoSocket = videoSocket_;
        packetDispatchState_->videoUsesTcp = false;
    }

    uint32_t negotiatedRefresh = clientConnect.refreshRateHz > 0
        ? clientConnect.refreshRateHz
        : refreshRateHz_;
    targetRefreshRateHz_.store(negotiatedRefresh);
    UpdatePredictionHorizon();

    state_.store(State::Connected);

    if (broadcastThread_.joinable())
    {
        broadcastThread_.join();
    }

    bool encoderReady = false;
    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        encoder_ = std::make_shared<VideoEncoder>();
        if (metalDevice_ != nullptr)
        {
            const ConfigValues config = Config::Get().GetValues();
            uint32_t bitrateMbps = (clientConnect.maxBitrateMbps > 0)
                ? std::min(config.bitrateMbps, clientConnect.maxBitrateMbps)
                : config.bitrateMbps;
            configMaxBitrateMbps_.store(bitrateMbps);
            currentBitrateMbps_.store(bitrateMbps);
            lastBitrateIncreaseTimeNs_ = SteadyClockNowNs();
            lastKeyframeRequestCountForAbr_ = 0;
            uint32_t stereoWidth = scaledWidth_ * 2;
            if (encoder_->Initialize(stereoWidth, scaledHeight_, negotiatedRefresh,
                                     bitrateMbps, metalDevice_))
            {
                encoder_->ForceKeyframe();
                frameIndex_ = 0;
                encoderReady = true;
                spdlog::info("StreamingServer: Client connected via WiFi: {} ({}:{}) refresh={}Hz",
                              clientName, clientIp_, clientPort_, negotiatedRefresh);
                RuntimeStatus::SetStreaming("wifi", clientName);
            }
            else
            {
                spdlog::error("StreamingServer: Failed to initialize VideoEncoder");
                encoder_.reset();
            }
        }
        else
        {
            spdlog::warn("StreamingServer: No Metal device set, cannot encode video");
            encoder_.reset();
        }
    }

    if (!encoderReady)
    {
        RuntimeStatus::SetIdle();
        state_.store(State::Broadcasting);
        if (wifiEnabled_ && running_.load())
        {
            broadcastThread_ = std::thread(&StreamingServer::BroadcastThread, this);
        }
    }

}

void StreamingServer::HandleUsbClientConnect(const oxr::protocol::ClientConnect& clientConnect)
{
    std::string clientName(clientConnect.deviceName,
        strnlen(clientConnect.deviceName, sizeof(clientConnect.deviceName)));
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        clientIp_ = "127.0.0.1";
        clientPort_ = oxr::protocol::CONTROL_PORT;
        clientName_ = clientName;
        clientUsesUsbAdb_ = true;
    }
    {
        std::lock_guard<std::mutex> tcpLock(tcpSocketMutex_);
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->clientIp.clear();
        packetDispatchState_->videoSocket = tcpVideoClientSocket_;
        packetDispatchState_->videoUsesTcp = true;
    }

    uint32_t negotiatedRefresh = clientConnect.refreshRateHz > 0
        ? clientConnect.refreshRateHz
        : refreshRateHz_;
    targetRefreshRateHz_.store(negotiatedRefresh);
    UpdatePredictionHorizon();

    state_.store(State::Connected);

    if (broadcastThread_.joinable())
    {
        broadcastThread_.join();
    }

    bool encoderReady = false;
    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        encoder_ = std::make_shared<VideoEncoder>();
        if (metalDevice_ != nullptr)
        {
            const ConfigValues config = Config::Get().GetValues();
            uint32_t bitrateMbps = (clientConnect.maxBitrateMbps > 0)
                ? std::min(config.bitrateMbps, clientConnect.maxBitrateMbps)
                : config.bitrateMbps;
            configMaxBitrateMbps_.store(bitrateMbps);
            currentBitrateMbps_.store(bitrateMbps);
            lastBitrateIncreaseTimeNs_ = SteadyClockNowNs();
            lastKeyframeRequestCountForAbr_ = 0;
            uint32_t stereoWidth = scaledWidth_ * 2;
            if (encoder_->Initialize(stereoWidth, scaledHeight_, negotiatedRefresh,
                                     bitrateMbps, metalDevice_))
            {
                encoder_->ForceKeyframe();
                frameIndex_ = 0;
                encoderReady = true;
                spdlog::info("StreamingServer: Client connected via usb_adb: {} refresh={}Hz",
                              clientName, negotiatedRefresh);
                RuntimeStatus::SetStreaming("usb_adb", clientName);
            }
            else
            {
                spdlog::error("StreamingServer: Failed to initialize VideoEncoder for USB ADB");
                encoder_.reset();
            }
        }
        else
        {
            spdlog::warn("StreamingServer: No Metal device set, cannot encode video");
            encoder_.reset();
        }
    }

    if (!encoderReady)
    {
        RuntimeStatus::SetIdle();
        clientUsesUsbAdb_ = false;
        state_.store(State::Broadcasting);
        if (wifiEnabled_ && running_.load())
        {
            broadcastThread_ = std::thread(&StreamingServer::BroadcastThread, this);
        }
    }
}

void StreamingServer::HandleClientDisconnect()
{
    std::lock_guard<std::mutex> disconnectLock(disconnectMutex_);
    if (!running_.load())
    {
        return;
    }

    State previousState = state_.exchange(State::Broadcasting);
    bool hadClient = false;
    bool wasUsbClient = false;
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        hadClient = clientUsesUsbAdb_ || !clientIp_.empty() ||
                    !clientName_.empty() || clientPort_ != 0;
        wasUsbClient = clientUsesUsbAdb_;
        clientIp_.clear();
        clientName_.clear();
        clientPort_ = 0;
        clientUsesUsbAdb_ = false;
    }
    if (previousState != State::Connected && !hadClient)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
        packetDispatchState_->clientIp.clear();
        packetDispatchState_->videoSocket = videoSocket_;
        packetDispatchState_->videoUsesTcp = false;
    }
    if (wasUsbClient)
    {
        std::lock_guard<std::mutex> lock(tcpSocketMutex_);
        CloseTcpSocket(tcpControlClientSocket_);
        CloseTcpSocket(tcpVideoClientSocket_);
        CloseTcpSocket(tcpTrackingClientSocket_);
    }

    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        ReleasePendingFrame(pendingFrame_);
    }

    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        encoder_.reset();
    }

    targetRefreshRateHz_.store(refreshRateHz_);
    UpdatePredictionHorizon();

    if (previousState == State::Connected && broadcastThread_.joinable())
    {
        broadcastThread_.join();
    }

    if (previousState == State::Connected && wifiEnabled_ && running_.load())
    {
        broadcastThread_ = std::thread(&StreamingServer::BroadcastThread, this);
    }

    RuntimeStatus::SetIdle();
    spdlog::info("StreamingServer: Client disconnected, resuming broadcast");
}

void StreamingServer::HandleLatencyReport(const oxr::protocol::LatencyReport& report)
{
    clientReceiveToSubmitMs_.store(report.receiveToDecoderSubmitMs);
    clientDecodeLatencyMs_.store(report.decodeLatencyMs);
    clientCompositorLatencyMs_.store(report.compositorLatencyMs);

    float previous = clientPipelineLatencyMs_.load();
    float smoothed = previous * 0.8f + report.totalClientLatencyMs * 0.2f;
    clientPipelineLatencyMs_.store(smoothed);
    UpdatePredictionHorizon();

    // Adaptive bitrate: adjust based on latency trends and keyframe request frequency
    {
        uint32_t currentBitrate = currentBitrateMbps_.load();
        int64_t nowNs = SteadyClockNowNs();
        uint32_t keyframeRequests = requestKeyframeCount_.load();
        uint32_t newKeyframeRequests = keyframeRequests - lastKeyframeRequestCountForAbr_;
        lastKeyframeRequestCountForAbr_ = keyframeRequests;

        // Target: keep total client latency under 30ms with no recent keyframe requests
        constexpr float kLatencyTargetMs = 30.0f;
        constexpr float kLatencyHighMs = 45.0f;
        constexpr uint32_t kMinBitrateMbps = 10;
        constexpr int64_t kIncreaseIntervalNs = 5LL * 1000000000; // 5 seconds

        bool shouldDecrease = (smoothed > kLatencyHighMs) || (newKeyframeRequests > 0);
        bool canIncrease = (smoothed < kLatencyTargetMs) && (newKeyframeRequests == 0) &&
                           (nowNs - lastBitrateIncreaseTimeNs_ >= kIncreaseIntervalNs) &&
                           (currentBitrate < configMaxBitrateMbps_.load());

        uint32_t newBitrate = currentBitrate;
        if (shouldDecrease && currentBitrate > kMinBitrateMbps)
        {
            newBitrate = std::max(currentBitrate * 9 / 10, kMinBitrateMbps); // -10%
        }
        else if (canIncrease)
        {
            newBitrate = std::min(currentBitrate + currentBitrate / 20,
                                  configMaxBitrateMbps_.load()); // +5%
            lastBitrateIncreaseTimeNs_ = nowNs;
        }

        if (newBitrate != currentBitrate)
        {
            currentBitrateMbps_.store(newBitrate);
            std::lock_guard<std::mutex> lock(encoderMutex_);
            if (encoder_ != nullptr)
            {
                encoder_->SetBitrate(newBitrate);
            }
        }
    }

    static auto lastLogTime = Clock::now();
    auto now = Clock::now();
    if (now - lastLogTime >= std::chrono::seconds(1))
    {
        spdlog::info("StreamingServer: client latency receive->submit={:.2f}ms decode={:.2f}ms compositor={:.2f}ms total={:.2f}ms horizon={:.2f}ms bitrate={}Mbps",
                      report.receiveToDecoderSubmitMs,
                      report.decodeLatencyMs,
                      report.compositorLatencyMs,
                      report.totalClientLatencyMs,
                      trackingReceiver_ ? trackingReceiver_->GetPredictionHorizonMs() : 0.0f,
                      currentBitrateMbps_.load());
        lastLogTime = now;
    }
}

void StreamingServer::HandleKeyframeRequest(const oxr::protocol::RequestKeyframe& request)
{
    requestKeyframeCount_.fetch_add(1);

    std::lock_guard<std::mutex> lock(encoderMutex_);
    if (encoder_ != nullptr)
    {
        encoder_->ForceKeyframe();
    }

    spdlog::info("StreamingServer: Keyframe requested (reasons=0x{:x}, detail={})",
                  request.reasonFlags, request.detail);
}

void StreamingServer::HandleControlPayload(const uint8_t* data, size_t size)
{
    if (data == nullptr || size < 1)
    {
        return;
    }

    uint8_t type = data[0];
    if (type == static_cast<uint8_t>(oxr::protocol::ControlType::LatencyReport) &&
        size >= sizeof(oxr::protocol::LatencyReport))
    {
        HandleLatencyReport(*reinterpret_cast<const oxr::protocol::LatencyReport*>(data));
    }
    else if (type == static_cast<uint8_t>(oxr::protocol::ControlType::RequestKeyframe) &&
             size >= sizeof(oxr::protocol::RequestKeyframe))
    {
        HandleKeyframeRequest(*reinterpret_cast<const oxr::protocol::RequestKeyframe*>(data));
    }
    else if (type == static_cast<uint8_t>(oxr::protocol::ControlType::NackRequest) &&
             size >= sizeof(oxr::protocol::NackRequest))
    {
        if (!clientUsesUsbAdb_)
        {
            HandleNackRequest(*reinterpret_cast<const oxr::protocol::NackRequest*>(data));
        }
    }
}

void StreamingServer::UpdatePredictionHorizon()
{
    if (trackingReceiver_ == nullptr)
    {
        return;
    }

    float horizonMs = serverPipelineLatencyMs_.load() + clientPipelineLatencyMs_.load();
    horizonMs = std::clamp(horizonMs, 0.0f, 80.0f);
    trackingReceiver_->SetPredictionHorizonMs(horizonMs);
}

void StreamingServer::SendFrame(void* leftTexture, void* rightTexture)
{
    if (leftTexture == nullptr || rightTexture == nullptr || state_.load() != State::Connected)
    {
        Swapchain::ReleaseTextureSlice(leftTexture);
        Swapchain::ReleaseTextureSlice(rightTexture);
        return;
    }

    {
        std::lock_guard<std::mutex> encoderLock(encoderMutex_);
        if (encoder_ == nullptr || !encoder_->IsInitialized())
        {
            Swapchain::ReleaseTextureSlice(leftTexture);
            Swapchain::ReleaseTextureSlice(rightTexture);
            return;
        }
    }

    PendingFrame replacedFrame = {};
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (pendingFrame_.valid)
        {
            replacedFrame = pendingFrame_;
            replacedFrameCount_.fetch_add(1);
        }

        pendingFrame_.leftTexture = leftTexture;
        pendingFrame_.rightTexture = rightTexture;
        pendingFrame_.frameIndex = frameIndex_++;
        pendingFrame_.timestampNs = SteadyClockNowNs();
        pendingFrame_.valid = true;
        pendingFrameDepthMax_.store(std::max(pendingFrameDepthMax_.load(), 1u));

        // Capture the predicted pose used for this frame's rendering
        if (trackingReceiver_ != nullptr)
        {
            oxr::protocol::TrackingPacket pose = {};
            if (trackingReceiver_->GetPredictedPose(pose))
            {
                memcpy(pendingFrame_.headPosition, pose.headPosition, sizeof(float) * 3);
                memcpy(pendingFrame_.headOrientation, pose.headOrientation, sizeof(float) * 4);
                pendingFrame_.hasPose = true;
            }
        }
    }

    ReleasePendingFrame(replacedFrame);
    frameReadyCv_.notify_one();
}

void StreamingServer::ReleasePendingFrame(PendingFrame& frame)
{
    if (!frame.valid)
    {
        return;
    }

    Swapchain::ReleaseTextureSlice(frame.leftTexture);
    Swapchain::ReleaseTextureSlice(frame.rightTexture);
    frame = {};
}

void StreamingServer::SendNalUnit(const std::shared_ptr<PacketDispatchState>& dispatchState,
                                  uint32_t frameIndex, const uint8_t* data, size_t size,
                                  bool isKeyframe, int64_t timestampNs)
{
    std::string clientIp;
    int videoSocket = -1;
    bool videoUsesTcp = false;
    {
        if (!dispatchState)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(dispatchState->mutex);
        if (!dispatchState->acceptingPackets)
        {
            return;
        }
        clientIp = dispatchState->clientIp;
        videoSocket = dispatchState->videoSocket;
        videoUsesTcp = dispatchState->videoUsesTcp;
    }

    if (videoSocket < 0 || (!videoUsesTcp && clientIp.empty()))
    {
        return;
    }

    if (videoUsesTcp)
    {
        if (size > oxr::protocol::TCP_MAX_RECORD_PAYLOAD - sizeof(oxr::protocol::TcpVideoNalHeader))
        {
            spdlog::warn("StreamingServer: USB TCP NAL too large ({} bytes), dropping", size);
            return;
        }

        std::vector<uint8_t> payload(sizeof(oxr::protocol::TcpVideoNalHeader) + size);
        oxr::protocol::TcpVideoNalHeader nalHeader = {};
        nalHeader.presentationTimeNs = timestampNs;
        nalHeader.frameIndex = frameIndex;
        nalHeader.payloadSize = static_cast<uint32_t>(size);
        nalHeader.flags = oxr::protocol::VIDEO_FLAG_STEREO;
        if (isKeyframe)
        {
            nalHeader.flags |= oxr::protocol::VIDEO_FLAG_KEYFRAME;
        }
        nalHeader.codec = static_cast<uint8_t>(oxr::protocol::VideoCodec::H265);
        memcpy(payload.data(), &nalHeader, sizeof(nalHeader));
        memcpy(payload.data() + sizeof(nalHeader), data, size);

        std::lock_guard<std::mutex> sendLock(dispatchState->sendMutex);
        SendTcpRecord(videoSocket, oxr::protocol::TcpRecordType::VideoNal,
                      payload.data(), payload.size());
        return;
    }

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(oxr::protocol::VIDEO_PORT);
    inet_pton(AF_INET, clientIp.c_str(), &destAddr.sin_addr);

    uint16_t totalPackets = static_cast<uint16_t>(
        (size + oxr::protocol::MAX_PACKET_PAYLOAD - 1) / oxr::protocol::MAX_PACKET_PAYLOAD);

    uint8_t packetBuffer[oxr::protocol::VIDEO_PACKET_SIZE];

    // Prepare cache slot for this frame's packets (for NACK retransmission)
    CachedFrame* cachedFrame = nullptr;
    {
        std::lock_guard<std::mutex> lock(dispatchState->mutex);
        size_t idx = dispatchState->cacheWriteIndex % PacketDispatchState::MaxCachedFrames;
        cachedFrame = &dispatchState->cachedFrames[idx];
        cachedFrame->frameIndex = frameIndex;
        cachedFrame->packets.clear();
        cachedFrame->packets.resize(totalPackets);
        dispatchState->cacheWriteIndex++;
    }

    // Collect per-packet payload pointers and sizes for FEC generation
    std::vector<const uint8_t*> payloadPtrs(totalPackets);
    std::vector<uint16_t> payloadSizes(totalPackets);

    size_t offset = 0;
    for (uint16_t i = 0; i < totalPackets; i++)
    {
        size_t payloadSize = std::min(size - offset, oxr::protocol::MAX_PACKET_PAYLOAD);
        payloadPtrs[i] = data + offset;
        payloadSizes[i] = static_cast<uint16_t>(payloadSize);

        oxr::protocol::VideoPacketHeader header = {};
        header.frameIndex = frameIndex;
        header.packetIndex = i;
        header.totalPackets = totalPackets;
        header.payloadSize = static_cast<uint16_t>(payloadSize);
        header.flags = oxr::protocol::VIDEO_FLAG_STEREO;
        if (isKeyframe)
        {
            header.flags |= oxr::protocol::VIDEO_FLAG_KEYFRAME;
        }
        if (i == totalPackets - 1)
        {
            header.flags |= oxr::protocol::VIDEO_FLAG_END_OF_FRAME;
        }
        header.codec = static_cast<uint8_t>(oxr::protocol::VideoCodec::H265);
        header.presentationTimeNs = timestampNs;

        size_t packetSize = sizeof(header) + payloadSize;
        memcpy(packetBuffer, &header, sizeof(header));
        memcpy(packetBuffer + sizeof(header), data + offset, payloadSize);
        sendto(videoSocket, packetBuffer, packetSize, MSG_DONTWAIT,
               (sockaddr*)&destAddr, sizeof(destAddr));

        // Cache for NACK retransmission
        cachedFrame->packets[i].data.assign(packetBuffer, packetBuffer + packetSize);

        offset += payloadSize;

        // After each complete FEC group (or at the end of the frame), send a parity packet
        uint32_t nextIdx = i + 1;
        bool isGroupEnd = (nextIdx % oxr::protocol::FEC_GROUP_SIZE == 0);
        bool isFrameEnd = (nextIdx == totalPackets);
        if (isGroupEnd || isFrameEnd)
        {
            uint32_t groupIndex = i / oxr::protocol::FEC_GROUP_SIZE;
            uint32_t groupStart = groupIndex * oxr::protocol::FEC_GROUP_SIZE;
            uint32_t groupCount = nextIdx - groupStart;

            uint8_t fecPayload[oxr::protocol::MAX_PACKET_PAYLOAD];
            oxr::fec::Encode(&payloadPtrs[groupStart], &payloadSizes[groupStart],
                             groupCount, fecPayload);

            oxr::protocol::VideoPacketHeader fecHeader = {};
            fecHeader.frameIndex = frameIndex;
            fecHeader.packetIndex = static_cast<uint16_t>(groupIndex);
            fecHeader.totalPackets = totalPackets;
            fecHeader.payloadSize = static_cast<uint16_t>(oxr::protocol::MAX_PACKET_PAYLOAD);
            fecHeader.flags = oxr::protocol::VIDEO_FLAG_FEC | oxr::protocol::VIDEO_FLAG_STEREO;
            if (isKeyframe)
            {
                fecHeader.flags |= oxr::protocol::VIDEO_FLAG_KEYFRAME;
            }
            fecHeader.codec = static_cast<uint8_t>(oxr::protocol::VideoCodec::H265);
            fecHeader.presentationTimeNs = timestampNs;

            memcpy(packetBuffer, &fecHeader, sizeof(fecHeader));
            memcpy(packetBuffer + sizeof(fecHeader), fecPayload, oxr::protocol::MAX_PACKET_PAYLOAD);
            sendto(videoSocket, packetBuffer,
                   sizeof(fecHeader) + oxr::protocol::MAX_PACKET_PAYLOAD, MSG_DONTWAIT,
                   (sockaddr*)&destAddr, sizeof(destAddr));
        }
    }
}

bool StreamingServer::SendTcpRecord(int socket, oxr::protocol::TcpRecordType type,
                                    const void* payload, size_t payloadSize)
{
    if (socket < 0 || payloadSize > oxr::protocol::TCP_MAX_RECORD_PAYLOAD)
    {
        return false;
    }

    oxr::protocol::TcpRecordHeader header = {};
    header.type = type;
    header.payloadSize = static_cast<uint32_t>(payloadSize);
    if (!SendAll(socket, &header, sizeof(header)))
    {
        return false;
    }
    if (payloadSize == 0)
    {
        return true;
    }
    return SendAll(socket, payload, payloadSize);
}

void StreamingServer::HandleNackRequest(const oxr::protocol::NackRequest& request)
{
    std::lock_guard<std::mutex> lock(packetDispatchState_->mutex);
    if (!packetDispatchState_->acceptingPackets || packetDispatchState_->clientIp.empty() ||
        packetDispatchState_->videoSocket < 0)
    {
        return;
    }

    // Find the cached frame
    const CachedFrame* cached = nullptr;
    for (size_t i = 0; i < PacketDispatchState::MaxCachedFrames; i++)
    {
        if (packetDispatchState_->cachedFrames[i].frameIndex == request.frameIndex &&
            !packetDispatchState_->cachedFrames[i].packets.empty())
        {
            cached = &packetDispatchState_->cachedFrames[i];
            break;
        }
    }

    if (cached == nullptr)
    {
        spdlog::debug("StreamingServer: NACK for frame {} but not in cache", request.frameIndex);
        return;
    }

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(oxr::protocol::VIDEO_PORT);
    inet_pton(AF_INET, packetDispatchState_->clientIp.c_str(), &destAddr.sin_addr);

    uint32_t retransmitted = 0;
    for (uint32_t bit = 0; bit < 64; bit++)
    {
        if (!(request.missingBitmask & (1ULL << bit)))
        {
            continue;
        }

        uint32_t pktIdx = request.packetIndexStart + bit;
        if (pktIdx >= cached->packets.size() || cached->packets[pktIdx].data.empty())
        {
            continue;
        }

        const auto& pkt = cached->packets[pktIdx];
        sendto(packetDispatchState_->videoSocket, pkt.data.data(), pkt.data.size(),
               MSG_DONTWAIT, (sockaddr*)&destAddr, sizeof(destAddr));
        retransmitted++;
    }

    if (retransmitted > 0)
    {
        spdlog::info("StreamingServer: NACK retransmitted {}/{} packets for frame {}",
                      retransmitted, __builtin_popcountll(request.missingBitmask),
                      request.frameIndex);
    }
}

std::string StreamingServer::GetClientName() const
{
    std::lock_guard<std::mutex> lock(clientMutex_);
    return clientName_;
}

std::string StreamingServer::GetLocalIpAddress() const
{
    struct ifaddrs* ifas = nullptr;
    if (getifaddrs(&ifas) != 0)
    {
        return "0.0.0.0";
    }

    std::string result = "0.0.0.0";
    for (struct ifaddrs* ifa = ifas; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        if (ifa->ifa_flags & IFF_LOOPBACK)
        {
            continue;
        }

        auto* addr = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));

        std::string ifName(ifa->ifa_name);
        if (ifName == "en0")
        {
            result = ipStr;
            break;
        }
        if (result == "0.0.0.0")
        {
            result = ipStr;
        }
    }

    freeifaddrs(ifas);
    return result;
}
