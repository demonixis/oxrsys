// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <oxrsys/protocol/FecCodec.h>
#include <oxrsys/protocol/Protocol.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace oxr
{
namespace streaming
{

// Reassembles UDP video packets into complete NAL units, recovering losses from XOR FEC parity.
// Shared by every UDP video client (android-vr, steam-frame) so the reassembly rules -- parity
// matched by totalPackets as well as frameIndex, negotiated group layouts, recovery on parity
// arrival -- exist and are tested exactly once; the receivers own sockets, NACKs, logging, and
// stats, and drive this from their receive thread. Single-threaded by design.
class VideoFrameAssembler
{
public:
    struct Frame
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        uint32_t frameIndex = 0;
        uint32_t totalPackets = 0;
        int64_t timestampNs = 0;
        uint8_t flags = 0;
        uint8_t codec = 0;
    };

    struct AbandonedFrame
    {
        uint32_t frameIndex = 0;
        uint32_t receivedPackets = 0;
        uint32_t totalPackets = 0;
        // Per-packet received flags (totalPackets entries), so the caller can NACK exactly the
        // missing ones. Valid only for the duration of the callback.
        const uint8_t* packetReceived = nullptr;
    };

    // Frame::data is valid only for the duration of the callback.
    using OnFrameCallback = std::function<void(const Frame&)>;
    using OnFrameAbandonedCallback = std::function<void(const AbandonedFrame&)>;
    using OnFecRecoveryCallback =
        std::function<void(uint32_t packetIndex, uint32_t totalPackets, uint32_t frameIndex)>;

    void SetOnFrame(OnFrameCallback callback) { onFrame_ = std::move(callback); }
    void SetOnFrameAbandoned(OnFrameAbandonedCallback callback)
    {
        onFrameAbandoned_ = std::move(callback);
    }
    void SetOnFecRecovery(OnFecRecoveryCallback callback)
    {
        onFecRecovery_ = std::move(callback);
    }

    // Selects the FEC group layout for frames that begin after this call; a frame in flight
    // keeps the layout it started with, so sender and receiver never mix layouts mid-frame.
    // Must match the server, which decides from CLIENT_CAPABILITY_FEC_INTERLEAVED.
    void SetInterleaved(bool interleaved) { interleaved_ = interleaved; }

    // Feed one video packet (data or FEC parity; not render-pose packets).
    void ProcessPacket(const protocol::VideoPacketHeader& header,
                       const uint8_t* payload, size_t payloadSize)
    {
        // FEC parity packet - store it separately. The server reuses one frameIndex for every
        // NAL of a frame, each with its own packet numbering, so frameIndex alone does not
        // identify the NAL this parity belongs to: totalPackets must match too, or a parity
        // packet from a small NAL (SPS/PPS) reordered behind the next NAL's data would occupy
        // that NAL's parity slot and XOR-recover garbage.
        if (header.flags & protocol::VIDEO_FLAG_FEC)
        {
            if (header.frameIndex == pending_.frameIndex && pending_.totalPackets > 0 &&
                header.totalPackets == pending_.totalPackets)
            {
                const uint32_t groupIdx = header.packetIndex;
                if (groupIdx < pending_.fecGroupCount && !pending_.fecReceived[groupIdx])
                {
                    const size_t fecOffset = groupIdx * protocol::MAX_PACKET_PAYLOAD;
                    memcpy(pending_.fecData.data() + fecOffset, payload,
                           std::min(payloadSize, protocol::MAX_PACKET_PAYLOAD));
                    pending_.fecGroupLastPacketSizes[groupIdx] =
                        header.fecGroupLastPacketPayloadSize;
                    pending_.fecReceived[groupIdx] = 1;
                    pending_.fecHeldCount++;

                    // Each parity packet can recover at most one loss (in its own group), so
                    // recovery is worth attempting as soon as the parity held could cover
                    // everything missing - not only when a single packet is missing, which
                    // would defer every burst recovery to the next frame's arrival.
                    const uint32_t missing =
                        pending_.totalPackets - pending_.receivedPackets;
                    if (missing > 0 && pending_.fecHeldCount >= missing)
                    {
                        if (TryFecRecovery())
                        {
                            DeliverPending();
                        }
                    }
                }
            }
            return;
        }

        // A different frameIndex - or a different totalPackets under the same frameIndex,
        // meaning another NAL of the same frame - starts a new reassembly.
        if (header.frameIndex != pending_.frameIndex ||
            header.totalPackets != pending_.totalPackets || pending_.totalPackets == 0)
        {
            AbandonOrRecoverPending();
            StartFrame(header);
        }

        if (header.packetIndex < header.totalPackets &&
            !pending_.packetReceived[header.packetIndex])
        {
            const size_t offset = header.packetIndex * protocol::MAX_PACKET_PAYLOAD;
            memcpy(pending_.data.data() + offset, payload, payloadSize);
            pending_.packetReceived[header.packetIndex] = 1;
            pending_.packetSizes[header.packetIndex] = static_cast<uint16_t>(payloadSize);
            pending_.receivedPackets++;

            if (pending_.receivedPackets == pending_.totalPackets)
            {
                DeliverPending();
            }
        }
    }

    // Drop any partial frame, e.g. between connections.
    void Reset()
    {
        pending_.totalPackets = 0;
        pending_.frameIndex = 0;
    }

private:
    struct PendingFrame
    {
        uint32_t frameIndex = 0;
        uint32_t totalPackets = 0;
        uint32_t receivedPackets = 0;
        int64_t timestampNs = 0;
        uint8_t flags = 0;
        uint8_t codec = static_cast<uint8_t>(protocol::VideoCodec::H265);
        bool interleaved = false;
        std::vector<uint8_t> data;
        std::vector<uint8_t> packetReceived;
        std::vector<uint16_t> packetSizes;  // Actual size of each packet's payload
        std::vector<uint8_t> compactedData;

        // FEC parity packets indexed by group number
        uint32_t fecGroupCount = 0;
        uint32_t fecHeldCount = 0;
        std::vector<uint8_t> fecReceived;
        std::vector<uint8_t> fecData;  // fecGroupCount * MAX_PACKET_PAYLOAD
        std::vector<uint16_t> fecGroupLastPacketSizes;
    };

    void StartFrame(const protocol::VideoPacketHeader& header)
    {
        pending_.frameIndex = header.frameIndex;
        pending_.totalPackets = header.totalPackets;
        pending_.receivedPackets = 0;
        pending_.timestampNs = header.presentationTimeNs;
        pending_.flags = header.flags;
        pending_.codec = header.codec;
        // Latched per frame: a layout change mid-frame (never expected from a well-behaved
        // server) must not mix layouts within one frame's recovery.
        pending_.interleaved = interleaved_;
        const size_t dataBytes = header.totalPackets * protocol::MAX_PACKET_PAYLOAD;
        if (pending_.data.size() < dataBytes)
        {
            pending_.data.resize(dataBytes);
        }
        pending_.packetReceived.assign(header.totalPackets, 0);
        pending_.packetSizes.assign(header.totalPackets, 0);

        pending_.fecGroupCount = fec::GroupCount(header.totalPackets);
        pending_.fecHeldCount = 0;
        pending_.fecReceived.assign(pending_.fecGroupCount, 0);
        const size_t fecBytes = pending_.fecGroupCount * protocol::MAX_PACKET_PAYLOAD;
        if (pending_.fecData.size() < fecBytes)
        {
            pending_.fecData.resize(fecBytes);
        }
        pending_.fecGroupLastPacketSizes.assign(pending_.fecGroupCount, 0);
    }

    void AbandonOrRecoverPending()
    {
        if (pending_.totalPackets == 0 || pending_.receivedPackets == 0 ||
            pending_.receivedPackets >= pending_.totalPackets)
        {
            return;
        }
        if (TryFecRecovery())
        {
            DeliverPending();
            return;
        }
        if (onFrameAbandoned_)
        {
            onFrameAbandoned_({pending_.frameIndex, pending_.receivedPackets,
                               pending_.totalPackets, pending_.packetReceived.data()});
        }
    }

    bool TryFecRecovery()
    {
        const uint32_t totalPackets = pending_.totalPackets;
        if (totalPackets == 0)
        {
            return false;
        }

        bool recovered = false;
        // Must match the layout the server used; negotiated via
        // CLIENT_CAPABILITY_FEC_INTERLEAVED.
        const fec::GroupLayout layout{totalPackets, pending_.interleaved};
        const uint32_t groupCount = layout.Count();

        for (uint32_t g = 0; g < groupCount; g++)
        {
            if (!pending_.fecReceived[g])
            {
                continue;  // No FEC parity for this group
            }

            const uint32_t members = layout.MemberCount(g);
            if (members == 0)
            {
                continue;
            }

            // Count missing packets in this group
            uint32_t missingIdx = UINT32_MAX;
            uint32_t missingCount = 0;
            for (uint32_t k = 0; k < members; k++)
            {
                const uint32_t i = layout.Member(g, k);
                if (!pending_.packetReceived[i])
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
            const uint32_t presentCount = members - 1;
            std::array<const uint8_t*, protocol::FEC_GROUP_SIZE> presentPtrs = {};
            std::array<uint16_t, protocol::FEC_GROUP_SIZE> presentSizes = {};
            uint32_t p = 0;
            for (uint32_t k = 0; k < members; k++)
            {
                const uint32_t i = layout.Member(g, k);
                if (i != missingIdx)
                {
                    presentPtrs[p] = pending_.data.data() + i * protocol::MAX_PACKET_PAYLOAD;
                    presentSizes[p] = pending_.packetSizes[i];
                    p++;
                }
            }

            const uint8_t* fecPayload =
                pending_.fecData.data() + g * protocol::MAX_PACKET_PAYLOAD;
            uint8_t* recoveredSlot =
                pending_.data.data() + missingIdx * protocol::MAX_PACKET_PAYLOAD;
            fec::Decode(presentPtrs.data(), presentSizes.data(), presentCount, fecPayload,
                        recoveredSlot);

            uint16_t recoveredSize = static_cast<uint16_t>(protocol::MAX_PACKET_PAYLOAD);
            if (missingIdx == layout.Member(g, members - 1))
            {
                const uint16_t groupLastPacketSize = pending_.fecGroupLastPacketSizes[g];
                if (groupLastPacketSize > 0 &&
                    groupLastPacketSize <= protocol::MAX_PACKET_PAYLOAD)
                {
                    recoveredSize = groupLastPacketSize;
                }
            }

            pending_.packetReceived[missingIdx] = 1;
            pending_.packetSizes[missingIdx] = recoveredSize;
            pending_.receivedPackets++;
            recovered = true;

            if (onFecRecovery_)
            {
                onFecRecovery_(missingIdx, totalPackets, pending_.frameIndex);
            }
        }

        return recovered && (pending_.receivedPackets == pending_.totalPackets);
    }

    void DeliverPending()
    {
        size_t totalSize = 0;
        for (uint32_t i = 0; i < pending_.totalPackets; i++)
        {
            totalSize += pending_.packetSizes[i];
        }

        Frame frame;
        frame.size = totalSize;
        frame.frameIndex = pending_.frameIndex;
        frame.totalPackets = pending_.totalPackets;
        frame.timestampNs = pending_.timestampNs;
        frame.flags = pending_.flags;
        frame.codec = pending_.codec;

        // Compact the data (remove gaps from fixed-size slots) into a reused buffer.
        if (pending_.totalPackets > 1)
        {
            pending_.compactedData.resize(totalSize);
            size_t dstOffset = 0;
            for (uint32_t i = 0; i < pending_.totalPackets; i++)
            {
                const size_t srcOffset = i * protocol::MAX_PACKET_PAYLOAD;
                memcpy(pending_.compactedData.data() + dstOffset,
                       pending_.data.data() + srcOffset,
                       pending_.packetSizes[i]);
                dstOffset += pending_.packetSizes[i];
            }
            frame.data = pending_.compactedData.data();
        }
        else
        {
            frame.data = pending_.data.data();
        }

        if (onFrame_)
        {
            onFrame_(frame);
        }

        pending_.totalPackets = 0;  // Mark as consumed
    }

    bool interleaved_ = false;
    PendingFrame pending_;
    OnFrameCallback onFrame_;
    OnFrameAbandonedCallback onFrameAbandoned_;
    OnFecRecoveryCallback onFecRecovery_;
};

} // namespace streaming
} // namespace oxr
