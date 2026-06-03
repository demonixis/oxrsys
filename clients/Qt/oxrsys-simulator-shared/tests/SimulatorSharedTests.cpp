// SPDX-License-Identifier: MPL-2.0

#include "SimulatorTracking.h"
#include "VideoFrameAssembler.h"

#include <oxrsys/protocol/FecCodec.h>

#include <QByteArray>
#include <QDebug>
#include <QSet>

#include <cstring>
#include <stdexcept>

namespace
{

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

oxr::protocol::VideoPacketHeader videoHeader(uint32_t frameIndex,
                                             uint16_t packetIndex,
                                             uint16_t totalPackets,
                                             uint16_t payloadSize,
                                             uint8_t flags = oxr::protocol::VIDEO_FLAG_STEREO)
{
    oxr::protocol::VideoPacketHeader header = {};
    header.frameIndex = frameIndex;
    header.packetIndex = packetIndex;
    header.totalPackets = totalPackets;
    header.payloadSize = payloadSize;
    header.flags = flags;
    header.codec = static_cast<uint8_t>(oxr::protocol::VideoCodec::H265);
    header.presentationTimeNs = 1234 + frameIndex;
    return header;
}

void testCompleteVideoFrame()
{
    VideoFrameAssembler assembler;
    const QByteArray first("abc", 3);
    const QByteArray second("de", 2);

    expect(assembler.addPacket(videoHeader(1, 0, 2, 3), first.constData(), first.size(), 10).isEmpty(),
           "Expected first packet to wait for frame completion");
    const QList<AssembledVideoFrame> frames =
        assembler.addPacket(videoHeader(1, 1, 2, 2), second.constData(), second.size(), 20);

    expect(frames.size() == 1, "Expected one complete frame");
    expect(frames.first().nalUnit == QByteArray("abcde", 5), "Expected packet payload concatenation");
    expect(frames.first().presentationTimeNs == 1235, "Expected presentation timestamp");
}

void testDuplicatePacketIgnored()
{
    VideoFrameAssembler assembler;
    const QByteArray first("abc", 3);
    const QByteArray duplicate("xxx", 3);
    const QByteArray second("de", 2);

    assembler.addPacket(videoHeader(1, 0, 2, 3), first.constData(), first.size(), 10);
    assembler.addPacket(videoHeader(1, 0, 2, 3), duplicate.constData(), duplicate.size(), 11);
    const QList<AssembledVideoFrame> frames =
        assembler.addPacket(videoHeader(1, 1, 2, 2), second.constData(), second.size(), 20);

    expect(frames.size() == 1, "Expected duplicate to keep one complete frame");
    expect(frames.first().nalUnit.startsWith("abc"), "Expected duplicate packet to be ignored");
}

void testIncompleteFrameDrop()
{
    VideoFrameAssembler assembler;
    const QByteArray first("abc", 3);
    assembler.addPacket(videoHeader(1, 0, 2, 3), first.constData(), first.size(), 10);
    assembler.addPacket(videoHeader(2, 0, 1, 3), first.constData(), first.size(), 20);
    expect(assembler.droppedFrames() == 1, "Expected incomplete previous frame to drop");
}

void testRenderPoseIgnored()
{
    VideoFrameAssembler assembler;
    const QByteArray payload("pose", 4);
    oxr::protocol::VideoPacketHeader header = videoHeader(
        1,
        0,
        0,
        static_cast<uint16_t>(payload.size()),
        oxr::protocol::VIDEO_FLAG_RENDER_POSE);
    expect(assembler.addPacket(header, payload.constData(), payload.size(), 10).isEmpty(),
           "Expected render pose metadata to be ignored");
    expect(assembler.droppedFrames() == 0, "Expected render pose to not count as a dropped frame");
}

void testFecRecovery()
{
    VideoFrameAssembler assembler;
    QByteArray first("aaaa", 4);
    QByteArray missing("bbbb", 4);
    QByteArray third("cccc", 4);
    const uint8_t* payloads[] = {
        reinterpret_cast<const uint8_t*>(first.constData()),
        reinterpret_cast<const uint8_t*>(missing.constData()),
        reinterpret_cast<const uint8_t*>(third.constData()),
    };
    const uint16_t payloadSizes[] = {4, 4, 4};
    QByteArray fec;
    fec.resize(static_cast<qsizetype>(oxr::protocol::MAX_PACKET_PAYLOAD));
    oxr::fec::Encode(payloads, payloadSizes, 3, reinterpret_cast<uint8_t*>(fec.data()));

    assembler.addPacket(videoHeader(7, 0, 3, 4), first.constData(), first.size(), 10);
    assembler.addPacket(videoHeader(7, 2, 3, 4), third.constData(), third.size(), 12);
    const QList<AssembledVideoFrame> frames =
        assembler.addPacket(videoHeader(7,
                                        0,
                                        3,
                                        static_cast<uint16_t>(oxr::protocol::MAX_PACKET_PAYLOAD),
                                        oxr::protocol::VIDEO_FLAG_FEC |
                                            oxr::protocol::VIDEO_FLAG_STEREO),
                            fec.constData(),
                            fec.size(),
                            13);

    expect(frames.size() == 1, "Expected FEC to complete the frame");
    expect(frames.first().recoveredWithFec, "Expected frame to be marked as FEC recovered");
    expect(assembler.fecRecoveries() == 1, "Expected one FEC recovery");
    expect(frames.first().nalUnit.mid(4, 4) == missing,
           "Expected recovered packet bytes at the missing packet offset");
}

void testTrackingFlagsAndMovementTargets()
{
    using namespace oxrsys::qt_simulator;
    SimulatorTrackingPose pose;

    QSet<int> keys;
    keys.insert(Qt::Key_W);
    advanceSimulatorTracking(pose, {}, keys, 1.0f);
    expect(pose.headPosition[2] < -1.9f, "Expected unmodified movement to move head");
    expect(pose.leftControllerPosition[2] == -0.4f,
           "Expected head movement to leave left controller in place");

    SimulatorTrackingPose shiftedPose;
    QSet<int> shiftedKeys;
    shiftedKeys.insert(Qt::Key_W);
    shiftedKeys.insert(LeftShiftKey);
    advanceSimulatorTracking(shiftedPose, {}, shiftedKeys, 1.0f);
    expect(shiftedPose.headPosition[2] == 0.0f,
           "Expected left-shift movement to leave head in place");
    expect(shiftedPose.leftControllerPosition[2] < -1.9f,
           "Expected left-shift movement to move left controller");

    SimulatorTrackingPose rightShiftedPose;
    QSet<int> rightShiftedKeys;
    rightShiftedKeys.insert(Qt::Key_W);
    rightShiftedKeys.insert(RightShiftKey);
    advanceSimulatorTracking(rightShiftedPose, {}, rightShiftedKeys, 1.0f);
    expect(rightShiftedPose.headPosition[2] == 0.0f,
           "Expected right-shift movement to leave head in place");
    expect(rightShiftedPose.rightControllerPosition[2] < -1.9f,
           "Expected right-shift movement to move right controller");

    oxr::protocol::TrackingPacket packet = {};
    fillSimulatorTrackingPacket(shiftedPose, shiftedKeys, 42, packet);
    expect((packet.trackingFlags & oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE) != 0,
           "Expected left controller active flag");
    expect((packet.trackingFlags & oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE) != 0,
           "Expected right controller active flag");
    expect(packet.timestampNs == 42, "Expected caller-provided monotonic timestamp");
}

} // namespace

int main()
{
    try
    {
        testCompleteVideoFrame();
        testDuplicatePacketIgnored();
        testIncompleteFrameDrop();
        testRenderPoseIgnored();
        testFecRecovery();
        testTrackingFlagsAndMovementTargets();
    }
    catch (const std::exception& error)
    {
        qWarning("Simulator shared tests failed: %s", error.what());
        return 1;
    }
    return 0;
}
