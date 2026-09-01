// SPDX-License-Identifier: MPL-2.0

#include "TrackingSender.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOG_TAG "OXRSys-Tracking"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace oxr
{

namespace
{

enum class SendResult
{
    Complete,
    WouldBlockBeforeWrite,
    Failed,
};

SendResult SendAll(int socket, const void* data, size_t size, bool& wroteAny)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t sentTotal = 0;
    while (sentTotal < size)
    {
        ssize_t sent = send(socket, bytes + sentTotal, size - sentTotal,
                            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return wroteAny ? SendResult::Failed : SendResult::WouldBlockBeforeWrite;
            }
            return SendResult::Failed;
        }
        if (sent == 0)
        {
            return SendResult::Failed;
        }
        wroteAny = true;
        sentTotal += static_cast<size_t>(sent);
    }
    return SendResult::Complete;
}

} // namespace

TrackingSender::~TrackingSender()
{
    Disconnect();
}

bool TrackingSender::Connect(const char* serverIp, uint16_t trackingPort)
{
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0)
    {
        LOGE("Failed to create tracking socket");
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(trackingPort);
    inet_pton(AF_INET, serverIp, &addr.sin_addr);

    // "Connect" the UDP socket for easier send()
    if (connect(socket_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        LOGE("Failed to connect tracking socket to %s:%d", serverIp, trackingPort);
        close(socket_);
        socket_ = -1;
        return false;
    }

    LOGI("Tracking sender connected to %s:%d", serverIp, trackingPort);
    return true;
}

void TrackingSender::Disconnect()
{
    if (socket_ >= 0)
    {
        close(socket_);
        socket_ = -1;
        usbMode_ = false;
        LOGI("Tracking sender disconnected");
    }
}

bool TrackingSender::ConnectTcp(uint16_t trackingPort)
{
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0)
    {
        LOGE("Failed to create TCP tracking socket");
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(trackingPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    LOGI("USB tracking: connecting to 127.0.0.1:%d via TCP...", trackingPort);

    if (connect(socket_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        LOGE("TCP tracking connect failed — is adb reverse set up?");
        close(socket_);
        socket_ = -1;
        return false;
    }

    // Disable Nagle's for low-latency tracking
    int nodelay = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int flags = fcntl(socket_, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
    }

    usbMode_ = true;
    LOGI("USB tracking sender connected via TCP to localhost:%d", trackingPort);
    return true;
}

bool TrackingSender::Send(const protocol::TrackingPacket& packet)
{
    if (socket_ < 0)
    {
        return false;
    }

    if (usbMode_)
    {
        protocol::TcpRecordHeader header = {};
        header.type = protocol::TcpRecordType::Tracking;
        header.payloadSize = sizeof(packet);
        bool wroteAny = false;
        const SendResult headerResult = SendAll(socket_, &header, sizeof(header), wroteAny);
        if (headerResult == SendResult::WouldBlockBeforeWrite)
        {
            return false;
        }
        if (headerResult == SendResult::Complete)
        {
            const SendResult packetResult = SendAll(socket_, &packet, sizeof(packet), wroteAny);
            if (packetResult == SendResult::Complete)
            {
                return true;
            }
        }

        if (wroteAny)
        {
            LOGE("USB tracking send failed after partial TCP record; closing socket");
        }
        else
        {
            LOGE("USB tracking send failed; closing TCP tracking socket");
        }
        Disconnect();
        return false;
    }
    else
    {
        ssize_t sent = send(socket_, &packet, sizeof(packet), MSG_DONTWAIT);
        return sent == sizeof(packet);
    }
}

} // namespace oxr
