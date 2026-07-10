// SPDX-License-Identifier: MPL-2.0

#include "VideoFrameAssembler.h"

#include <oxrsys/protocol/FecCodec.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace
{

constexpr uint16_t MaxFramePackets = 4096;

qsizetype packetOffset(uint16_t packetIndex)
{
    return static_cast<qsizetype>(packetIndex) *
        static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD);
}

} // namespace

QList<AssembledVideoFrame> VideoFrameAssembler::addPacket(
    const oxr::protocol::VideoPacketHeader& header,
    const char* payload,
    qsizetype payloadSize,
    int64_t receiveTimeNs)
{
    QList<AssembledVideoFrame> completedFrames;

    if ((header.flags & oxr::protocol::VIDEO_FLAG_RENDER_POSE) != 0)
    {
        return completedFrames;
    }
    if (header.totalPackets == 0 || header.packetIndex >= header.totalPackets ||
        header.totalPackets > MaxFramePackets)
    {
        return completedFrames;
    }

    payloadSize = std::min<qsizetype>(
        payloadSize,
        static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD));

    const bool fecPacket = (header.flags & oxr::protocol::VIDEO_FLAG_FEC) != 0;
    const bool newFrame =
        pendingTotalPackets_ == 0 || header.frameIndex != pendingFrameIndex_;
    if (newFrame)
    {
        completedFrames.append(finishPendingFrame(true, receiveTimeNs));
        startFrame(header, receiveTimeNs);
    }

    pendingLastPacketTimeNs_ = receiveTimeNs;

    if (fecPacket)
    {
        const uint32_t groupIndex = header.packetIndex;
        if (groupIndex >= static_cast<uint32_t>(pendingFecReceived_.size()))
        {
            return completedFrames;
        }
        if (pendingFecReceived_[static_cast<int>(groupIndex)] != 0)
        {
            return completedFrames;
        }

        const qsizetype offset = packetOffset(static_cast<uint16_t>(groupIndex));
        if (offset < 0 ||
            offset + static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD) >
                pendingFecData_.size())
        {
            return completedFrames;
        }

        std::memset(pendingFecData_.data() + offset, 0, oxr::protocol::MAX_PACKET_PAYLOAD);
        if (payload != nullptr && payloadSize > 0)
        {
            std::memcpy(pendingFecData_.data() + offset,
                        payload,
                        static_cast<size_t>(payloadSize));
        }
        pendingFecReceived_[static_cast<int>(groupIndex)] = 1;
        pendingFecGroupLastPacketSizes_[static_cast<int>(groupIndex)] =
            header.fecGroupLastPacketPayloadSize;

        if (tryFecRecovery() && isComplete())
        {
            completedFrames.append(deliverPendingFrame(receiveTimeNs));
            reset();
        }
        return completedFrames;
    }

    if (pendingPacketReceived_[header.packetIndex] != 0)
    {
        return completedFrames;
    }

    const qsizetype offset = packetOffset(header.packetIndex);
    if (offset < 0 || offset + payloadSize > pendingFrameData_.size())
    {
        return completedFrames;
    }

    if (payload != nullptr && payloadSize > 0)
    {
        std::memcpy(pendingFrameData_.data() + offset,
                    payload,
                    static_cast<size_t>(payloadSize));
    }
    pendingPacketReceived_[header.packetIndex] = 1;
    pendingPacketSizes_[header.packetIndex] = static_cast<uint16_t>(payloadSize);
    ++pendingReceivedPackets_;

    if (isComplete())
    {
        completedFrames.append(deliverPendingFrame(receiveTimeNs));
        reset();
    }
    else if (tryFecRecovery() && isComplete())
    {
        completedFrames.append(deliverPendingFrame(receiveTimeNs));
        reset();
    }

    return completedFrames;
}

QList<AssembledVideoFrame> VideoFrameAssembler::expirePendingFrame(int64_t nowNs,
                                                                   int64_t timeoutNs)
{
    if (pendingTotalPackets_ == 0 || pendingLastPacketTimeNs_ <= 0 ||
        nowNs - pendingLastPacketTimeNs_ <= timeoutNs)
    {
        return {};
    }
    return finishPendingFrame(true, nowNs);
}

void VideoFrameAssembler::reset()
{
    pendingFrameIndex_ = UINT32_MAX;
    pendingTotalPackets_ = 0;
    pendingReceivedPackets_ = 0;
    pendingPresentationTimeNs_ = 0;
    pendingLastPacketTimeNs_ = 0;
    pendingCodec_ = oxr::protocol::VideoCodec::H265;
    pendingFrameData_.clear();
    pendingPacketSizes_.clear();
    pendingPacketReceived_.clear();
    pendingFecData_.clear();
    pendingFecReceived_.clear();
    pendingFecGroupLastPacketSizes_.clear();
    pendingRecoveredWithFec_ = false;
}

quint64 VideoFrameAssembler::droppedFrames() const
{
    return droppedFrames_;
}

quint64 VideoFrameAssembler::fecRecoveries() const
{
    return fecRecoveries_;
}

void VideoFrameAssembler::startFrame(const oxr::protocol::VideoPacketHeader& header,
                                     int64_t receiveTimeNs)
{
    pendingFrameIndex_ = header.frameIndex;
    pendingTotalPackets_ = header.totalPackets;
    pendingReceivedPackets_ = 0;
    pendingPresentationTimeNs_ = header.presentationTimeNs;
    pendingLastPacketTimeNs_ = receiveTimeNs;
    pendingCodec_ = static_cast<oxr::protocol::VideoCodec>(header.codec);
    pendingRecoveredWithFec_ = false;
    pendingFrameData_.fill(0, static_cast<qsizetype>(pendingTotalPackets_) *
                                  static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD));
    pendingPacketSizes_.fill(0, pendingTotalPackets_);
    pendingPacketReceived_.fill(0, pendingTotalPackets_);

    const uint32_t fecGroups = oxr::fec::GroupCount(pendingTotalPackets_);
    pendingFecData_.fill(0, static_cast<qsizetype>(fecGroups) *
                                static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD));
    pendingFecReceived_.fill(0, static_cast<int>(fecGroups));
    pendingFecGroupLastPacketSizes_.fill(0, static_cast<int>(fecGroups));
}

QList<AssembledVideoFrame> VideoFrameAssembler::finishPendingFrame(bool countDropIfIncomplete,
                                                                   int64_t receiveTimeNs)
{
    QList<AssembledVideoFrame> completedFrames;
    if (pendingTotalPackets_ == 0)
    {
        return completedFrames;
    }
    if (isComplete() || (tryFecRecovery() && isComplete()))
    {
        completedFrames.append(deliverPendingFrame(receiveTimeNs));
    }
    else if (countDropIfIncomplete)
    {
        ++droppedFrames_;
    }
    reset();
    return completedFrames;
}

bool VideoFrameAssembler::tryFecRecovery()
{
    if (pendingTotalPackets_ == 0 || pendingFecReceived_.isEmpty())
    {
        return false;
    }

    bool recoveredAnyPacket = false;
    for (int groupIndex = 0; groupIndex < pendingFecReceived_.size(); ++groupIndex)
    {
        if (pendingFecReceived_[groupIndex] == 0)
        {
            continue;
        }

        uint32_t groupStart = 0;
        uint32_t groupEnd = 0;
        oxr::fec::GroupRange(static_cast<uint32_t>(groupIndex),
                             pendingTotalPackets_,
                             groupStart,
                             groupEnd);

        int missingIndex = -1;
        int missingCount = 0;
        for (uint32_t packetIndex = groupStart; packetIndex < groupEnd; ++packetIndex)
        {
            if (pendingPacketReceived_[static_cast<int>(packetIndex)] == 0)
            {
                missingIndex = static_cast<int>(packetIndex);
                ++missingCount;
            }
        }
        if (missingCount != 1)
        {
            continue;
        }

        uint8_t* destination = reinterpret_cast<uint8_t*>(
            pendingFrameData_.data() + packetOffset(static_cast<uint16_t>(missingIndex)));
        const uint8_t* fecPayload = reinterpret_cast<const uint8_t*>(
            pendingFecData_.constData() + packetOffset(static_cast<uint16_t>(groupIndex)));
        std::memcpy(destination, fecPayload, oxr::protocol::MAX_PACKET_PAYLOAD);

        for (uint32_t packetIndex = groupStart; packetIndex < groupEnd; ++packetIndex)
        {
            if (static_cast<int>(packetIndex) == missingIndex)
            {
                continue;
            }
            const uint16_t packetSize =
                pendingPacketSizes_[static_cast<int>(packetIndex)];
            const uint8_t* source = reinterpret_cast<const uint8_t*>(
                pendingFrameData_.constData() + packetOffset(static_cast<uint16_t>(packetIndex)));
            for (uint16_t byteIndex = 0; byteIndex < packetSize; ++byteIndex)
            {
                destination[byteIndex] ^= source[byteIndex];
            }
        }

        pendingPacketReceived_[missingIndex] = 1;
        uint16_t recoveredSize = static_cast<uint16_t>(oxr::protocol::MAX_PACKET_PAYLOAD);
        if (missingIndex == static_cast<int>(groupEnd - 1))
        {
            const uint16_t groupLastPacketSize =
                pendingFecGroupLastPacketSizes_[groupIndex];
            if (groupLastPacketSize > 0 &&
                groupLastPacketSize <= oxr::protocol::MAX_PACKET_PAYLOAD)
            {
                recoveredSize = groupLastPacketSize;
            }
        }
        pendingPacketSizes_[missingIndex] = recoveredSize;
        ++pendingReceivedPackets_;
        ++fecRecoveries_;
        pendingRecoveredWithFec_ = true;
        recoveredAnyPacket = true;
    }
    return recoveredAnyPacket;
}

bool VideoFrameAssembler::isComplete() const
{
    return pendingTotalPackets_ > 0 && pendingReceivedPackets_ == pendingTotalPackets_;
}

AssembledVideoFrame VideoFrameAssembler::deliverPendingFrame(int64_t receiveTimeNs) const
{
    qsizetype totalSize = 0;
    for (uint16_t packetSize : pendingPacketSizes_)
    {
        totalSize += packetSize;
    }

    QByteArray nalUnit;
    nalUnit.resize(totalSize);
    qsizetype destinationOffset = 0;
    for (int i = 0; i < pendingPacketSizes_.size(); ++i)
    {
        const qsizetype packetSize = pendingPacketSizes_[i];
        if (packetSize <= 0)
        {
            continue;
        }
        const qsizetype sourceOffset = packetOffset(static_cast<uint16_t>(i));
        std::memcpy(nalUnit.data() + destinationOffset,
                    pendingFrameData_.constData() + sourceOffset,
                    static_cast<size_t>(packetSize));
        destinationOffset += packetSize;
    }

    return {
        nalUnit,
        pendingFrameIndex_,
        pendingPresentationTimeNs_,
        receiveTimeNs,
        pendingCodec_,
        pendingRecoveredWithFec_,
    };
}
