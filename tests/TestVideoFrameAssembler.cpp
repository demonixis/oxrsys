// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <oxrsys/streaming/VideoFrameAssembler.h>

#include <oxrsys/protocol/FecCodec.h>
#include <oxrsys/protocol/Protocol.h>

#include <array>
#include <cstring>
#include <vector>

using namespace oxr;
using oxr::streaming::VideoFrameAssembler;

namespace
{

// A synthetic NAL: per-packet payloads with deterministic bytes and a short last packet, plus
// the parity packets a server emitting with the given layout would produce. Feeding these to
// the assembler in various orders and with various losses exercises the receiver half of the
// FEC contract without sockets.
struct TestNal
{
    uint32_t frameIndex;
    uint32_t totalPackets;
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<std::vector<uint8_t>> parity;
    std::vector<uint16_t> parityLastMemberSize;

    std::vector<uint8_t> ExpectedBytes() const
    {
        std::vector<uint8_t> all;
        for (const auto& p : payloads)
        {
            all.insert(all.end(), p.begin(), p.end());
        }
        return all;
    }
};

TestNal MakeNal(uint32_t frameIndex, uint32_t totalPackets, bool interleaved,
                size_t lastPacketSize = 313)
{
    TestNal nal;
    nal.frameIndex = frameIndex;
    nal.totalPackets = totalPackets;
    for (uint32_t i = 0; i < totalPackets; i++)
    {
        const size_t size =
            (i == totalPackets - 1) ? lastPacketSize : protocol::MAX_PACKET_PAYLOAD;
        std::vector<uint8_t> payload(size);
        for (size_t j = 0; j < size; j++)
        {
            payload[j] = static_cast<uint8_t>((frameIndex * 31 + i * 7 + j) % 251);
        }
        nal.payloads.push_back(std::move(payload));
    }

    const fec::GroupLayout layout{totalPackets, interleaved};
    for (uint32_t g = 0; g < layout.Count(); g++)
    {
        const uint32_t members = layout.MemberCount(g);
        std::array<const uint8_t*, protocol::FEC_GROUP_SIZE> ptrs = {};
        std::array<uint16_t, protocol::FEC_GROUP_SIZE> sizes = {};
        for (uint32_t k = 0; k < members; k++)
        {
            const uint32_t idx = layout.Member(g, k);
            ptrs[k] = nal.payloads[idx].data();
            sizes[k] = static_cast<uint16_t>(nal.payloads[idx].size());
        }
        std::vector<uint8_t> parity(protocol::MAX_PACKET_PAYLOAD);
        fec::Encode(ptrs.data(), sizes.data(), members, parity.data());
        nal.parity.push_back(std::move(parity));
        nal.parityLastMemberSize.push_back(sizes[members - 1]);
    }
    return nal;
}

protocol::VideoPacketHeader DataHeader(const TestNal& nal, uint32_t packetIndex)
{
    protocol::VideoPacketHeader header = {};
    header.frameIndex = nal.frameIndex;
    header.packetIndex = static_cast<uint16_t>(packetIndex);
    header.totalPackets = static_cast<uint16_t>(nal.totalPackets);
    header.payloadSize = static_cast<uint16_t>(nal.payloads[packetIndex].size());
    header.flags = protocol::VIDEO_FLAG_STEREO;
    header.presentationTimeNs = 1000 + nal.frameIndex;
    return header;
}

protocol::VideoPacketHeader ParityHeader(const TestNal& nal, uint32_t groupIndex)
{
    protocol::VideoPacketHeader header = {};
    header.frameIndex = nal.frameIndex;
    header.packetIndex = static_cast<uint16_t>(groupIndex);
    header.totalPackets = static_cast<uint16_t>(nal.totalPackets);
    header.payloadSize = static_cast<uint16_t>(protocol::MAX_PACKET_PAYLOAD);
    header.flags = protocol::VIDEO_FLAG_FEC | protocol::VIDEO_FLAG_STEREO;
    header.fecGroupLastPacketPayloadSize = nal.parityLastMemberSize[groupIndex];
    header.presentationTimeNs = 1000 + nal.frameIndex;
    return header;
}

struct Harness
{
    VideoFrameAssembler assembler;
    std::vector<std::vector<uint8_t>> deliveredFrames;
    std::vector<uint32_t> deliveredIndices;
    std::vector<VideoFrameAssembler::AbandonedFrame> abandoned;
    uint32_t fecRecoveries = 0;

    explicit Harness(bool interleaved)
    {
        assembler.SetInterleaved(interleaved);
        assembler.SetOnFrame([this](const VideoFrameAssembler::Frame& frame) {
            deliveredFrames.emplace_back(frame.data, frame.data + frame.size);
            deliveredIndices.push_back(frame.frameIndex);
        });
        assembler.SetOnFrameAbandoned(
            [this](const VideoFrameAssembler::AbandonedFrame& frame) {
                abandoned.push_back(frame);
            });
        assembler.SetOnFecRecovery(
            [this](uint32_t, uint32_t, uint32_t) { fecRecoveries++; });
    }

    void Data(const TestNal& nal, uint32_t packetIndex)
    {
        assembler.ProcessPacket(DataHeader(nal, packetIndex),
                                nal.payloads[packetIndex].data(),
                                nal.payloads[packetIndex].size());
    }

    void Parity(const TestNal& nal, uint32_t groupIndex)
    {
        assembler.ProcessPacket(ParityHeader(nal, groupIndex),
                                nal.parity[groupIndex].data(),
                                nal.parity[groupIndex].size());
    }
};

} // namespace

TEST_CASE("Lossless frames deliver once with intact bytes under both layouts",
          "[fec][assembler]")
{
    for (const bool interleaved : {false, true})
    {
        Harness harness(interleaved);
        const TestNal nal = MakeNal(7, 25, interleaved);
        for (uint32_t i = 0; i < nal.totalPackets; i++)
        {
            harness.Data(nal, i);
        }
        REQUIRE(harness.deliveredFrames.size() == 1);
        REQUIRE(harness.deliveredFrames[0] == nal.ExpectedBytes());
        REQUIRE(harness.abandoned.empty());
        REQUIRE(harness.fecRecoveries == 0);
    }
}

TEST_CASE("An adjacent burst is recovered when the parity arrives, not a frame later",
          "[fec][assembler]")
{
    Harness harness(true);
    const TestNal nal = MakeNal(9, 25, true);
    // Adjacent losses: the headline case interleaving exists for. With the old
    // "almost complete" gate this recovery was deferred to the next frame's first packet.
    for (uint32_t i = 0; i < nal.totalPackets; i++)
    {
        if (i == 10 || i == 11)
        {
            continue;
        }
        harness.Data(nal, i);
    }
    REQUIRE(harness.deliveredFrames.empty());
    const fec::GroupLayout layout{nal.totalPackets, true};
    harness.Parity(nal, layout.GroupOf(10));
    REQUIRE(harness.deliveredFrames.empty());  // one parity covers only one of the two losses
    harness.Parity(nal, layout.GroupOf(11));

    // Delivered by the parity itself: no packet of any later frame was needed.
    REQUIRE(harness.deliveredFrames.size() == 1);
    REQUIRE(harness.deliveredFrames[0] == nal.ExpectedBytes());
    REQUIRE(harness.fecRecoveries == 2);
    REQUIRE(harness.abandoned.empty());
}

TEST_CASE("A burst as long as the group count is recovered; one longer is not",
          "[fec][assembler]")
{
    const uint32_t totalPackets = 30;
    const fec::GroupLayout layout{totalPackets, true};
    const uint32_t groupCount = layout.Count();

    {
        Harness harness(true);
        const TestNal nal = MakeNal(11, totalPackets, true);
        for (uint32_t i = 0; i < totalPackets; i++)
        {
            if (i >= 5 && i < 5 + groupCount)
            {
                continue;
            }
            harness.Data(nal, i);
        }
        for (uint32_t g = 0; g < groupCount; g++)
        {
            harness.Parity(nal, g);
        }
        REQUIRE(harness.deliveredFrames.size() == 1);
        REQUIRE(harness.deliveredFrames[0] == nal.ExpectedBytes());
        REQUIRE(harness.fecRecoveries == groupCount);
    }

    {
        Harness harness(true);
        const TestNal nal = MakeNal(12, totalPackets, true);
        for (uint32_t i = 0; i < totalPackets; i++)
        {
            if (i >= 5 && i < 5 + groupCount + 1)
            {
                continue;
            }
            harness.Data(nal, i);
        }
        for (uint32_t g = 0; g < groupCount; g++)
        {
            harness.Parity(nal, g);
        }
        REQUIRE(harness.deliveredFrames.empty());

        // The next frame's first packet abandons the unrecoverable frame (NACK path) but must
        // still be stored for its own frame: losing it would turn one lost frame into two.
        const TestNal next = MakeNal(13, 3, true);
        harness.Data(next, 0);
        REQUIRE(harness.abandoned.size() == 1);
        REQUIRE(harness.abandoned[0].frameIndex == 12);
        harness.Data(next, 1);
        harness.Data(next, 2);
        REQUIRE(harness.deliveredFrames.size() == 1);
        REQUIRE(harness.deliveredFrames[0] == next.ExpectedBytes());
        REQUIRE(harness.deliveredIndices[0] == 13);
    }
}

TEST_CASE("Contiguous groups recover a single loss as soon as their parity arrives",
          "[fec][assembler]")
{
    Harness harness(false);
    const TestNal nal = MakeNal(21, 25, false);
    for (uint32_t i = 0; i < nal.totalPackets; i++)
    {
        if (i == 4)
        {
            continue;
        }
        harness.Data(nal, i);
        // Inline schedule: parity follows its group's last data packet.
        const uint32_t next = i + 1;
        if (next % protocol::FEC_GROUP_SIZE == 0 || next == nal.totalPackets)
        {
            harness.Parity(nal, i / protocol::FEC_GROUP_SIZE);
        }
    }
    REQUIRE(harness.deliveredFrames.size() == 1);
    REQUIRE(harness.deliveredFrames[0] == nal.ExpectedBytes());
    REQUIRE(harness.fecRecoveries == 1);
}

TEST_CASE("A recovered short last packet keeps its true size", "[fec][assembler]")
{
    for (const bool interleaved : {false, true})
    {
        Harness harness(interleaved);
        const TestNal nal = MakeNal(31, 25, interleaved, 47);
        const uint32_t last = nal.totalPackets - 1;
        for (uint32_t i = 0; i < nal.totalPackets; i++)
        {
            if (i != last)
            {
                harness.Data(nal, i);
            }
        }
        const fec::GroupLayout layout{nal.totalPackets, interleaved};
        harness.Parity(nal, layout.GroupOf(last));
        REQUIRE(harness.deliveredFrames.size() == 1);
        REQUIRE(harness.deliveredFrames[0] == nal.ExpectedBytes());
    }
}

TEST_CASE("Parity from another NAL sharing the frameIndex is rejected", "[fec][assembler]")
{
    // The server reuses one frameIndex across a frame's NALs (SPS/PPS then IDR), each numbering
    // packets and groups from zero. A reordered parity packet from the small NAL must not
    // occupy the large NAL's parity slot; totalPackets is the discriminator.
    Harness harness(true);
    const TestNal small = MakeNal(40, 1, true, 21);
    const TestNal large = MakeNal(40, 25, true);

    harness.Data(small, 0);
    REQUIRE(harness.deliveredFrames.size() == 1);

    // Large NAL starts; drop packet 0 (a group-0 member under both layouts).
    for (uint32_t i = 1; i < large.totalPackets; i++)
    {
        harness.Data(large, i);
    }
    // The small NAL's parity arrives late, reordered behind the large NAL's data.
    harness.Parity(small, 0);
    REQUIRE(harness.deliveredFrames.size() == 1);  // must not "recover" from foreign parity

    // The large NAL's own group-0 parity must still be accepted and recover correctly.
    const fec::GroupLayout layout{large.totalPackets, true};
    harness.Parity(large, layout.GroupOf(0));
    REQUIRE(harness.deliveredFrames.size() == 2);
    REQUIRE(harness.deliveredFrames[1] == large.ExpectedBytes());
}

TEST_CASE("A new NAL with the same frameIndex but different totalPackets flushes the previous",
          "[fec][assembler]")
{
    Harness harness(false);
    const TestNal first = MakeNal(50, 25, false);
    const TestNal second = MakeNal(50, 3, false, 99);

    harness.Data(first, 0);
    harness.Data(first, 1);
    harness.Data(second, 0);
    REQUIRE(harness.abandoned.size() == 1);
    REQUIRE(harness.abandoned[0].frameIndex == 50);
    REQUIRE(harness.abandoned[0].totalPackets == 25);

    harness.Data(second, 1);
    harness.Data(second, 2);
    REQUIRE(harness.deliveredFrames.size() == 1);
    REQUIRE(harness.deliveredFrames[0] == second.ExpectedBytes());
}

TEST_CASE("Duplicate data and parity packets are ignored", "[fec][assembler]")
{
    Harness harness(true);
    const TestNal nal = MakeNal(60, 12, true);
    for (uint32_t i = 0; i + 1 < nal.totalPackets; i++)
    {
        harness.Data(nal, i);
        harness.Data(nal, i);
    }
    const fec::GroupLayout layout{nal.totalPackets, true};
    const uint32_t lastGroup = layout.GroupOf(nal.totalPackets - 1);
    harness.Parity(nal, lastGroup);
    harness.Parity(nal, lastGroup);
    REQUIRE(harness.deliveredFrames.size() == 1);
    REQUIRE(harness.deliveredFrames[0] == nal.ExpectedBytes());
    REQUIRE(harness.fecRecoveries == 1);
}
