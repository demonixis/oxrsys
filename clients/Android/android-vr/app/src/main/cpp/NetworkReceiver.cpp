// SPDX-License-Identifier: MPL-2.0

#include "NetworkReceiver.h"
#include <oxrsys/protocol/FecCodec.h>

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
    receiving_.store(true);
    receiveThread_ = std::thread(&NetworkReceiver::ReceiveThread, this, nalCallback_);
    LOGI("Video receiver started on port %d", videoPort);
    return true;
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

        ReassembleFrame(*header, payload, payloadSize);
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

void NetworkReceiver::ReassembleFrame(const protocol::VideoPacketHeader& header,
                                      const uint8_t* payload, size_t payloadSize)
{
    // FEC parity packet — store it separately
    if (header.flags & protocol::VIDEO_FLAG_FEC)
    {
        if (header.frameIndex == pendingFrame_.frameIndex && pendingFrame_.totalPackets > 0)
        {
            uint32_t groupIdx = header.packetIndex;
            if (groupIdx < pendingFrame_.fecGroupCount && !pendingFrame_.fecReceived[groupIdx])
            {
                size_t fecOffset = groupIdx * protocol::MAX_PACKET_PAYLOAD;
                memcpy(pendingFrame_.fecData.data() + fecOffset, payload,
                       std::min(payloadSize, protocol::MAX_PACKET_PAYLOAD));
                pendingFrame_.fecGroupLastPacketSizes[groupIdx] =
                    header.fecGroupLastPacketPayloadSize;
                pendingFrame_.fecReceived[groupIdx] = 1;

                // Try FEC recovery if frame is almost complete
                if (pendingFrame_.receivedPackets + 1 >= pendingFrame_.totalPackets)
                {
                    if (TryFecRecovery())
                    {
                        goto deliver;
                    }
                }
            }
        }
        return;
    }

    // New frame?
    if (header.frameIndex != pendingFrame_.frameIndex ||
        pendingFrame_.totalPackets == 0)
    {
        // If we had a partial frame, try FEC recovery before dropping
        if (pendingFrame_.totalPackets > 0 && pendingFrame_.receivedPackets > 0 &&
            pendingFrame_.receivedPackets < pendingFrame_.totalPackets)
        {
            if (!TryFecRecovery())
            {
                // Send NACK for missing packets (server may retransmit from cache)
                SendNack(pendingFrame_.frameIndex, pendingFrame_.totalPackets);

                uint32_t dropped = framesDropped_.fetch_add(1) + 1;
                if (dropped <= 5 || dropped % 100 == 0)
                {
                    LOGI("Frame %u dropped (%u/%u packets received)",
                         pendingFrame_.frameIndex, pendingFrame_.receivedPackets,
                         pendingFrame_.totalPackets);
                }
            }
            else
            {
                goto deliver;
            }
        }

        pendingFrame_.frameIndex = header.frameIndex;
        pendingFrame_.totalPackets = header.totalPackets;
        pendingFrame_.receivedPackets = 0;
        pendingFrame_.timestampNs = header.presentationTimeNs;
        pendingFrame_.flags = header.flags;
        pendingFrame_.codec = header.codec;
        const size_t dataBytes = header.totalPackets * protocol::MAX_PACKET_PAYLOAD;
        if (pendingFrame_.data.size() < dataBytes)
        {
            pendingFrame_.data.resize(dataBytes);
        }
        pendingFrame_.packetReceived.assign(header.totalPackets, 0);
        pendingFrame_.packetSizes.assign(header.totalPackets, 0);

        // Initialize FEC tracking
        pendingFrame_.fecGroupCount = fec::GroupCount(header.totalPackets);
        pendingFrame_.fecReceived.assign(pendingFrame_.fecGroupCount, 0);
        const size_t fecBytes = pendingFrame_.fecGroupCount * protocol::MAX_PACKET_PAYLOAD;
        if (pendingFrame_.fecData.size() < fecBytes)
        {
            pendingFrame_.fecData.resize(fecBytes);
        }
        pendingFrame_.fecGroupLastPacketSizes.assign(pendingFrame_.fecGroupCount, 0);
    }

    // Store this packet's data
    if (header.packetIndex < header.totalPackets &&
        !pendingFrame_.packetReceived[header.packetIndex])
    {
        size_t offset = header.packetIndex * protocol::MAX_PACKET_PAYLOAD;
        memcpy(pendingFrame_.data.data() + offset, payload, payloadSize);
        pendingFrame_.packetReceived[header.packetIndex] = 1;
        pendingFrame_.packetSizes[header.packetIndex] = static_cast<uint16_t>(payloadSize);
        pendingFrame_.receivedPackets++;

        // Frame complete?
        if (pendingFrame_.receivedPackets == pendingFrame_.totalPackets)
        {
            goto deliver;
        }
    }
    return;

deliver:
    {
        // Calculate actual total size using tracked per-packet sizes
        size_t totalSize = 0;
        for (uint32_t i = 0; i < pendingFrame_.totalPackets; i++)
        {
            totalSize += pendingFrame_.packetSizes[i];
        }

        uint32_t delivered = framesDelivered_.fetch_add(1) + 1;
        int64_t receiveTimeNs = SteadyClockNowNs();
        lastCompletedFrameReceiveTimeNs_.store(receiveTimeNs);
        if (delivered <= 5 || delivered % 300 == 0)
        {
            LOGI("Frame %u complete: %u packets, %zu bytes total",
                 pendingFrame_.frameIndex, pendingFrame_.totalPackets, totalSize);
        }

        // Compact the data (remove gaps from fixed-size slots) into a reused buffer.
        if (pendingFrame_.totalPackets > 1)
        {
            pendingFrame_.compactedData.resize(totalSize);
            size_t dstOffset = 0;
            for (uint32_t i = 0; i < pendingFrame_.totalPackets; i++)
            {
                size_t srcOffset = i * protocol::MAX_PACKET_PAYLOAD;
                memcpy(pendingFrame_.compactedData.data() + dstOffset,
                       pendingFrame_.data.data() + srcOffset,
                       pendingFrame_.packetSizes[i]);
                dstOffset += pendingFrame_.packetSizes[i];
            }

            if (nalCallback_)
            {
                nalCallback_(pendingFrame_.compactedData.data(), totalSize,
                             pendingFrame_.timestampNs, receiveTimeNs,
                             pendingFrame_.flags, pendingFrame_.codec);
            }
        }
        else
        {
            if (nalCallback_)
            {
                nalCallback_(pendingFrame_.data.data(), totalSize,
                             pendingFrame_.timestampNs, receiveTimeNs,
                             pendingFrame_.flags, pendingFrame_.codec);
            }
        }

        pendingFrame_.totalPackets = 0;  // Mark as consumed
    }
}

bool NetworkReceiver::TryFecRecovery()
{
    uint32_t totalPackets = pendingFrame_.totalPackets;
    if (totalPackets == 0)
    {
        return false;
    }

    bool recovered = false;
    uint32_t groupCount = fec::GroupCount(totalPackets);

    for (uint32_t g = 0; g < groupCount; g++)
    {
        if (!pendingFrame_.fecReceived[g])
        {
            continue;  // No FEC parity for this group
        }

        uint32_t groupStart, groupEnd;
        fec::GroupRange(g, totalPackets, groupStart, groupEnd);

        // Count missing packets in this group
        uint32_t missingIdx = UINT32_MAX;
        uint32_t missingCount = 0;
        for (uint32_t i = groupStart; i < groupEnd; i++)
        {
            if (!pendingFrame_.packetReceived[i])
            {
                missingIdx = i;
                missingCount++;
            }
        }

        if (missingCount != 1)
        {
            continue;  // FEC can only recover exactly 1 missing packet per group
        }

        // Gather present packets for XOR recovery
        uint32_t presentCount = (groupEnd - groupStart) - 1;
        std::array<const uint8_t*, protocol::FEC_GROUP_SIZE> presentPtrs = {};
        std::array<uint16_t, protocol::FEC_GROUP_SIZE> presentSizes = {};
        uint32_t p = 0;
        for (uint32_t i = groupStart; i < groupEnd; i++)
        {
            if (i != missingIdx)
            {
                presentPtrs[p] = pendingFrame_.data.data() + i * protocol::MAX_PACKET_PAYLOAD;
                presentSizes[p] = pendingFrame_.packetSizes[i];
                p++;
            }
        }

        const uint8_t* fecPayload = pendingFrame_.fecData.data() + g * protocol::MAX_PACKET_PAYLOAD;
        uint8_t* recoveredSlot = pendingFrame_.data.data() + missingIdx * protocol::MAX_PACKET_PAYLOAD;
        fec::Decode(presentPtrs.data(), presentSizes.data(), presentCount, fecPayload, recoveredSlot);

        uint16_t recoveredSize = static_cast<uint16_t>(protocol::MAX_PACKET_PAYLOAD);
        if (missingIdx == groupEnd - 1)
        {
            const uint16_t groupLastPacketSize = pendingFrame_.fecGroupLastPacketSizes[g];
            if (groupLastPacketSize > 0 && groupLastPacketSize <= protocol::MAX_PACKET_PAYLOAD)
            {
                recoveredSize = groupLastPacketSize;
            }
        }

        pendingFrame_.packetReceived[missingIdx] = 1;
        pendingFrame_.packetSizes[missingIdx] = recoveredSize;
        pendingFrame_.receivedPackets++;
        recovered = true;

        uint32_t recoveries = fecRecoveries_.fetch_add(1) + 1;
        if (recoveries <= 10 || recoveries % 100 == 0)
        {
            LOGI("FEC recovered packet %u/%u in frame %u (recovery #%u)",
                 missingIdx, totalPackets, pendingFrame_.frameIndex, recoveries);
        }
    }

    return recovered && (pendingFrame_.receivedPackets == pendingFrame_.totalPackets);
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

void NetworkReceiver::SendNack(uint32_t frameIndex, uint32_t totalPackets)
{
    if (controlSocket_ < 0 || serverIp_.empty() || totalPackets == 0)
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
            if (!pendingFrame_.packetReceived[i])
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
