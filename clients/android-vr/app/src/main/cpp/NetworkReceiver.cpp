// SPDX-License-Identifier: MPL-2.0

#include "NetworkReceiver.h"

#include <android/log.h>
#include <array>
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <iterator>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <sys/socket.h>
#include <unistd.h>

#define LOG_TAG "OXRSys-Network"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace oxr
{

namespace
{

int64_t SteadyClockNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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

bool ReadTcpRecord(int socket, protocol::TcpRecordHeader& header, std::vector<uint8_t>& payload)
{
    if (!ReadAll(socket, &header, sizeof(header)))
    {
        return false;
    }
    if (header.magic != protocol::TCP_RECORD_MAGIC ||
        header.version != protocol::TCP_RECORD_VERSION ||
        header.payloadSize > protocol::TCP_MAX_RECORD_PAYLOAD)
    {
        return false;
    }

    payload.clear();
    payload.resize(header.payloadSize);
    if (payload.empty())
    {
        return true;
    }
    return ReadAll(socket, payload.data(), payload.size());
}

void ConfigureTcpSocket(int socket)
{
    int nodelay = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

} // namespace

NetworkReceiver::~NetworkReceiver()
{
    Stop();
}

bool NetworkReceiver::StartDiscovery(OnServerFoundCallback callback)
{
    discoverySocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (discoverySocket_ < 0)
    {
        LOGE("Failed to create discovery socket");
        return false;
    }

    // Allow reuse and broadcast
    int opt = 1;
    setsockopt(discoverySocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(discoverySocket_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(protocol::DISCOVERY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(discoverySocket_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        LOGE("Failed to bind discovery socket on port %d", protocol::DISCOVERY_PORT);
        close(discoverySocket_);
        discoverySocket_ = -1;
        return false;
    }

    discovering_.store(true);
    discoveryThread_ = std::thread(&NetworkReceiver::DiscoveryThread, this, std::move(callback));
    LOGI("Discovery started on port %d", protocol::DISCOVERY_PORT);
    return true;
}

void NetworkReceiver::DiscoveryThread(OnServerFoundCallback callback)
{
    uint8_t buffer[sizeof(protocol::ServerAnnounce)];

    while (discovering_.load())
    {
        // Set a timeout so we can check the running flag
        timeval tv = {1, 0};
        setsockopt(discoverySocket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Use recvfrom() to capture the server's IP address
        sockaddr_in senderAddr = {};
        socklen_t addrLen = sizeof(senderAddr);
        ssize_t received = recvfrom(discoverySocket_, buffer, sizeof(buffer), 0,
                                     (sockaddr*)&senderAddr, &addrLen);
        if (received >= (ssize_t)protocol::SERVER_ANNOUNCE_BASE_SIZE)
        {
            protocol::ServerAnnounce hello = {};
            memcpy(&hello, buffer, std::min<size_t>(static_cast<size_t>(received), sizeof(hello)));
            if (hello.type == protocol::MessageType::ServerAnnounce)
            {
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &senderAddr.sin_addr, ipStr, sizeof(ipStr));

                LOGI("Server found: %s at %s (%ux%u @ %uHz)",
                     hello.serverName, ipStr, hello.renderWidth,
                     hello.renderHeight, hello.refreshRateHz);
                callback(hello, ipStr);
            }
        }
    }
}

bool NetworkReceiver::StartReceiving(const char* serverIp, uint16_t videoPort,
                                     OnNalUnitCallback callback,
                                     OnConnectionLostCallback connectionLostCallback)
{
    (void)serverIp;

    videoSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (videoSocket_ < 0)
    {
        LOGE("Failed to create video socket");
        return false;
    }

    // Increase receive buffer for high bitrate video
    int bufferSize = 4 * 1024 * 1024;  // 4 MB
    setsockopt(videoSocket_, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(videoPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(videoSocket_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        LOGE("Failed to bind video socket on port %d", videoPort);
        close(videoSocket_);
        videoSocket_ = -1;
        return false;
    }

    nalCallback_ = std::move(callback);
    connectionLostCallback_ = std::move(connectionLostCallback);
    assembler_.Reset();
    WireAssemblerCallbacks();
    receiving_.store(true);
    receiveThread_ = std::thread(&NetworkReceiver::ReceiveThread, this, nalCallback_);
    LOGI("Video receiver started on port %d", videoPort);
    return true;
}

void NetworkReceiver::WireAssemblerCallbacks()
{
    assembler_.SetOnFrame([this](const streaming::VideoFrameAssembler::Frame& frame) {
        uint32_t delivered = framesDelivered_.fetch_add(1) + 1;
        int64_t receiveTimeNs = SteadyClockNowNs();
        lastCompletedFrameReceiveTimeNs_.store(receiveTimeNs);
        if (delivered <= 5 || delivered % 300 == 0)
        {
            LOGI("Frame %u complete: %u packets, %zu bytes total",
                 frame.frameIndex, frame.totalPackets, frame.size);
        }
        if (nalCallback_)
        {
            nalCallback_(frame.data, frame.size, frame.timestampNs, receiveTimeNs,
                         frame.flags, frame.codec);
        }
    });
    assembler_.SetOnFrameAbandoned([this](const streaming::VideoFrameAssembler::AbandonedFrame& frame) {
        // Send NACK for missing packets (server may retransmit from cache)
        SendNack(frame.frameIndex, frame.totalPackets, frame.packetReceived);

        uint32_t dropped = framesDropped_.fetch_add(1) + 1;
        if (dropped <= 5 || dropped % 100 == 0)
        {
            LOGI("Frame %u dropped (%u/%u packets received)",
                 frame.frameIndex, frame.receivedPackets, frame.totalPackets);
        }
    });
    assembler_.SetOnFecRecovery(
        [this](uint32_t packetIndex, uint32_t totalPackets, uint32_t frameIndex) {
            uint32_t recoveries = fecRecoveries_.fetch_add(1) + 1;
            if (recoveries <= 10 || recoveries % 100 == 0)
            {
                LOGI("FEC recovered packet %u/%u in frame %u (recovery #%u)",
                     packetIndex, totalPackets, frameIndex, recoveries);
            }
        });
}

bool NetworkReceiver::StartReceivingTcp(uint16_t videoPort, OnNalUnitCallback callback,
                                        OnConnectionLostCallback connectionLostCallback)
{
    videoSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (videoSocket_ < 0)
    {
        LOGE("Failed to create USB TCP video socket");
        return false;
    }
    ConfigureTcpSocket(videoSocket_);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(videoPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(videoSocket_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        LOGE("Failed to connect USB TCP video socket to 127.0.0.1:%d", videoPort);
        close(videoSocket_);
        videoSocket_ = -1;
        return false;
    }

    nalCallback_ = std::move(callback);
    connectionLostCallback_ = std::move(connectionLostCallback);
    receiving_.store(true);
    receiveThread_ = std::thread(&NetworkReceiver::ReceiveTcpThread, this, nalCallback_);
    LOGI("USB TCP video receiver connected to localhost:%d", videoPort);
    return true;
}

void NetworkReceiver::StopDiscovery()
{
    discovering_.store(false);
    // Don't close socket or join thread here — may be called from the discovery thread itself
    LOGI("Discovery stop requested");
}

void NetworkReceiver::ReceiveThread(OnNalUnitCallback callback)
{
    uint8_t buffer[protocol::VIDEO_PACKET_SIZE];
    LOGI("Video receive thread started");

    timeval tv = {0, 1000};  // 1ms timeout — low latency packet receive
    setsockopt(videoSocket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (receiving_.load())
    {
        ssize_t received = recv(videoSocket_, buffer, sizeof(buffer), 0);
        if (received < (ssize_t)sizeof(protocol::VideoPacketHeader))
        {
            continue;
        }

        auto* header = reinterpret_cast<protocol::VideoPacketHeader*>(buffer);
        const uint8_t* payload = buffer + sizeof(protocol::VideoPacketHeader);
        size_t payloadSize = received - sizeof(protocol::VideoPacketHeader);

        if (payloadSize > header->payloadSize)
        {
            payloadSize = header->payloadSize;
        }

        uint32_t packetCount = packetsReceived_.fetch_add(1) + 1;
        if (packetCount <= 5 || packetCount % 500 == 0)
        {
            LOGI("Video packet #%u: frame=%u pkt=%u/%u payload=%zu flags=0x%02x codec=%u",
                 packetCount, header->frameIndex, header->packetIndex,
                 header->totalPackets, payloadSize, header->flags, header->codec);
        }

        // Handle render pose packets (server sends these before each frame)
        if (header->flags & protocol::VIDEO_FLAG_RENDER_POSE)
        {
            StoreRenderPose(*header, payload, payloadSize);
            continue;
        }

        assembler_.SetInterleaved(fecInterleaved_.load());
        assembler_.ProcessPacket(*header, payload, payloadSize);
    }

    LOGI("Video receive thread ended (packets=%u frames=%u dropped=%u)",
         packetsReceived_.load(), framesDelivered_.load(), framesDropped_.load());
}

void NetworkReceiver::ReceiveTcpThread(OnNalUnitCallback callback)
{
    LOGI("USB TCP video receive thread started");
    std::vector<uint8_t> payload;
    while (receiving_.load())
    {
        protocol::TcpRecordHeader header = {};
        if (!ReadTcpRecord(videoSocket_, header, payload))
        {
            break;
        }

        if (header.type == protocol::TcpRecordType::RenderPose)
        {
            if (payload.size() >= sizeof(protocol::TcpRenderPose))
            {
                StoreRenderPose(*reinterpret_cast<const protocol::TcpRenderPose*>(payload.data()));
            }
            continue;
        }

        if (header.type != protocol::TcpRecordType::VideoNal ||
            payload.size() < sizeof(protocol::TcpVideoNalHeader))
        {
            continue;
        }

        const auto* nalHeader = reinterpret_cast<const protocol::TcpVideoNalHeader*>(payload.data());
        const uint8_t* nalData = payload.data() + sizeof(protocol::TcpVideoNalHeader);
        size_t nalSize = payload.size() - sizeof(protocol::TcpVideoNalHeader);
        if (nalSize > nalHeader->payloadSize)
        {
            nalSize = nalHeader->payloadSize;
        }

        uint32_t packetCount = packetsReceived_.fetch_add(1) + 1;
        uint32_t delivered = framesDelivered_.fetch_add(1) + 1;
        int64_t receiveTimeNs = SteadyClockNowNs();
        lastCompletedFrameReceiveTimeNs_.store(receiveTimeNs);
        if (packetCount <= 5 || packetCount % 500 == 0)
        {
            LOGI("USB TCP NAL #%u: frame=%u payload=%zu flags=0x%02x codec=%u delivered=%u",
                 packetCount, nalHeader->frameIndex, nalSize, nalHeader->flags,
                 nalHeader->codec, delivered);
        }

        if (callback)
        {
            callback(nalData, nalSize, nalHeader->presentationTimeNs, receiveTimeNs,
                     nalHeader->flags, nalHeader->codec);
        }
    }

    bool unexpectedDisconnect = receiving_.exchange(false);
    LOGI("USB TCP video receive thread ended (packets=%u records=%u)",
         packetsReceived_.load(), framesDelivered_.load());
    if (unexpectedDisconnect && connectionLostCallback_)
    {
        connectionLostCallback_("USB TCP video socket closed");
    }
}

void NetworkReceiver::StoreRenderPose(const protocol::VideoPacketHeader& header,
                                      const uint8_t* payload, size_t payloadSize)
{
    if (payloadSize < 7 * sizeof(float))
    {
        return;
    }

    const float* poseData = reinterpret_cast<const float*>(payload);
    RenderPose pose = {};
    pose.frameIndex = header.frameIndex;
    pose.presentationTimeNs = header.presentationTimeNs;
    pose.presentationTimeUs = header.presentationTimeNs / 1000;
    memcpy(pose.position, poseData, sizeof(float) * 3);
    memcpy(pose.orientation, poseData + 3, sizeof(float) * 4);
    pose.valid = true;

    std::lock_guard<std::mutex> lock(renderPoseMutex_);
    latestRenderPose_ = pose;

    for (RenderPose& existing : renderPoses_)
    {
        if (existing.presentationTimeUs == pose.presentationTimeUs)
        {
            existing = pose;
            return;
        }
    }

    renderPoses_.push_back(pose);
    while (renderPoses_.size() > MaxRenderPoses)
    {
        renderPoses_.pop_front();
    }
}

void NetworkReceiver::StoreRenderPose(const protocol::TcpRenderPose& tcpPose)
{
    RenderPose pose = {};
    pose.frameIndex = tcpPose.frameIndex;
    pose.presentationTimeNs = tcpPose.presentationTimeNs;
    pose.presentationTimeUs = tcpPose.presentationTimeNs / 1000;
    memcpy(pose.position, tcpPose.position, sizeof(float) * 3);
    memcpy(pose.orientation, tcpPose.orientation, sizeof(float) * 4);
    pose.valid = true;

    std::lock_guard<std::mutex> lock(renderPoseMutex_);
    latestRenderPose_ = pose;

    for (RenderPose& existing : renderPoses_)
    {
        if (existing.presentationTimeUs == pose.presentationTimeUs)
        {
            existing = pose;
            return;
        }
    }

    renderPoses_.push_back(pose);
    while (renderPoses_.size() > MaxRenderPoses)
    {
        renderPoses_.pop_front();
    }
}

NetworkReceiver::RenderPose NetworkReceiver::GetLatestRenderPose() const
{
    std::lock_guard<std::mutex> lock(renderPoseMutex_);
    return latestRenderPose_;
}

bool NetworkReceiver::TakeRenderPoseForPresentationTimeUs(int64_t presentationTimeUs,
                                                          RenderPose* outPose)
{
    std::lock_guard<std::mutex> lock(renderPoseMutex_);

    for (auto it = renderPoses_.begin(); it != renderPoses_.end(); ++it)
    {
        if (it->presentationTimeUs == presentationTimeUs)
        {
            if (outPose != nullptr)
            {
                *outPose = *it;
            }
            renderPoses_.erase(renderPoses_.begin(), std::next(it));
            return true;
        }
    }

    while (!renderPoses_.empty() &&
           renderPoses_.front().presentationTimeUs < presentationTimeUs)
    {
        renderPoses_.pop_front();
    }
    return false;
}

void NetworkReceiver::SetControlSocket(int socket, const char* serverIp)
{
    controlSocket_ = socket;
    serverIp_ = serverIp ? serverIp : "";
}

void NetworkReceiver::SendNack(uint32_t frameIndex, uint32_t totalPackets,
                               const uint8_t* packetReceived)
{
    if (controlSocket_ < 0 || serverIp_.empty() || totalPackets == 0 ||
        packetReceived == nullptr)
    {
        return;
    }

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(protocol::CONTROL_PORT);
    inet_pton(AF_INET, serverIp_.c_str(), &destAddr.sin_addr);

    // Send NACK requests for each 64-bit chunk of missing packets
    for (uint32_t start = 0; start < totalPackets; start += 64)
    {
        uint64_t bitmask = 0;
        uint32_t end = std::min(start + 64u, totalPackets);
        bool anyMissing = false;

        for (uint32_t i = start; i < end; i++)
        {
            if (!packetReceived[i])
            {
                bitmask |= (1ULL << (i - start));
                anyMissing = true;
            }
        }

        if (!anyMissing)
        {
            continue;
        }

        protocol::NackRequest nack = {};
        nack.type = protocol::ControlType::NackRequest;
        nack.frameIndex = frameIndex;
        nack.packetIndexStart = static_cast<uint16_t>(start);
        nack.totalPackets = static_cast<uint16_t>(totalPackets);
        nack.missingBitmask = bitmask;
        sendto(controlSocket_, &nack, sizeof(nack), MSG_DONTWAIT,
               (sockaddr*)&destAddr, sizeof(destAddr));

        uint32_t sent = nacksSent_.fetch_add(1) + 1;
        if (sent <= 10 || sent % 100 == 0)
        {
            LOGI("NACK sent for frame %u: missing %d packets (chunk %u)",
                 frameIndex, __builtin_popcountll(bitmask), start / 64);
        }
    }
}

void NetworkReceiver::Stop()
{
    discovering_.store(false);
    receiving_.store(false);

    if (discoverySocket_ >= 0)
    {
        close(discoverySocket_);
        discoverySocket_ = -1;
    }
    if (videoSocket_ >= 0)
    {
        close(videoSocket_);
        videoSocket_ = -1;
    }

    if (discoveryThread_.joinable())
    {
        discoveryThread_.join();
    }
    if (receiveThread_.joinable())
    {
        receiveThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(renderPoseMutex_);
        latestRenderPose_ = {};
        renderPoses_.clear();
    }
    connectionLostCallback_ = nullptr;

    LOGI("Network receiver stopped");
}

} // namespace oxr
