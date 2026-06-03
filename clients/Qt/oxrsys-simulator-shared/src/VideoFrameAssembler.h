// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QByteArray>
#include <QList>
#include <QVector>

#include <cstdint>

#include <oxrsys/protocol/Protocol.h>

struct AssembledVideoFrame
{
    QByteArray nalUnit;
    uint32_t frameIndex = 0;
    int64_t presentationTimeNs = 0;
    int64_t receiveTimeNs = 0;
    bool recoveredWithFec = false;
};

class VideoFrameAssembler final
{
public:
    QList<AssembledVideoFrame> addPacket(const oxr::protocol::VideoPacketHeader& header,
                                         const char* payload,
                                         qsizetype payloadSize,
                                         int64_t receiveTimeNs);
    QList<AssembledVideoFrame> expirePendingFrame(int64_t nowNs, int64_t timeoutNs);
    void reset();

    quint64 droppedFrames() const;
    quint64 fecRecoveries() const;

private:
    void startFrame(const oxr::protocol::VideoPacketHeader& header, int64_t receiveTimeNs);
    QList<AssembledVideoFrame> finishPendingFrame(bool countDropIfIncomplete,
                                                  int64_t receiveTimeNs);
    bool tryFecRecovery();
    bool isComplete() const;
    AssembledVideoFrame deliverPendingFrame(int64_t receiveTimeNs) const;

    uint32_t pendingFrameIndex_ = UINT32_MAX;
    uint16_t pendingTotalPackets_ = 0;
    uint16_t pendingReceivedPackets_ = 0;
    int64_t pendingPresentationTimeNs_ = 0;
    int64_t pendingLastPacketTimeNs_ = 0;
    QByteArray pendingFrameData_;
    QVector<uint16_t> pendingPacketSizes_;
    QVector<uint8_t> pendingPacketReceived_;
    QByteArray pendingFecData_;
    QVector<uint8_t> pendingFecReceived_;
    bool pendingRecoveredWithFec_ = false;
    quint64 droppedFrames_ = 0;
    quint64 fecRecoveries_ = 0;
};
