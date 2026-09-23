// SPDX-License-Identifier: MPL-2.0

#pragma once

// FecCodec.h — XOR-based Forward Error Correction for video packet streams.
//
// Shared between server (encoder) and client (decoder).
// Each FEC group covers up to FEC_GROUP_SIZE consecutive data packets.
// One parity packet is generated per group by XOR-ing all data payloads.
// If exactly 1 data packet in a group is lost, it can be recovered by
// XOR-ing the parity packet with the remaining data packets.

#include <oxrsys/protocol/Protocol.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace oxr
{
namespace fec
{

// Generate a FEC parity packet for a group of data payloads.
// Each payload is padded to MAX_PACKET_PAYLOAD for XOR alignment.
// outParity must point to a buffer of at least MAX_PACKET_PAYLOAD bytes.
inline void Encode(const uint8_t* const* payloads, const uint16_t* payloadSizes,
                   uint32_t count, uint8_t* outParity)
{
    memset(outParity, 0, protocol::MAX_PACKET_PAYLOAD);
    for (uint32_t i = 0; i < count; i++)
    {
        // XOR the actual payload bytes
        for (uint16_t j = 0; j < payloadSizes[i]; j++)
        {
            outParity[j] ^= payloads[i][j];
        }
        // Remaining bytes (payloadSizes[i]..MAX_PACKET_PAYLOAD) XOR with 0 = no-op
    }
}

// Recover a single missing data packet from a FEC group.
// presentPayloads/presentSizes: the received data packets in the group (count-1 of them).
// fecPayload: the FEC parity packet (MAX_PACKET_PAYLOAD bytes).
// outRecovered: buffer of at least MAX_PACKET_PAYLOAD bytes for the recovered packet.
// Returns the recovered payload size (MAX_PACKET_PAYLOAD; caller should use the
// original payloadSize from the header if known, or MAX_PACKET_PAYLOAD otherwise).
inline uint16_t Decode(const uint8_t* const* presentPayloads, const uint16_t* presentSizes,
                       uint32_t presentCount, const uint8_t* fecPayload,
                       uint8_t* outRecovered)
{
    // Start with the FEC parity
    memcpy(outRecovered, fecPayload, protocol::MAX_PACKET_PAYLOAD);
    // XOR out each present packet to isolate the missing one
    for (uint32_t i = 0; i < presentCount; i++)
    {
        for (uint16_t j = 0; j < presentSizes[i]; j++)
        {
            outRecovered[j] ^= presentPayloads[i][j];
        }
    }
    return protocol::MAX_PACKET_PAYLOAD;
}

// Compute how many FEC groups a frame with totalDataPackets needs.
inline uint32_t GroupCount(uint32_t totalDataPackets)
{
    return (totalDataPackets + protocol::FEC_GROUP_SIZE - 1) / protocol::FEC_GROUP_SIZE;
}

// How a frame's data packets are assigned to FEC groups.
//
// Contiguous is the original layout: group g owns packets [g*SIZE, g*SIZE+SIZE). One XOR parity
// per group recovers one loss per group, so any two *adjacent* losses land in the same group and
// are unrecoverable -- which is the common case on Wi-Fi, where loss arrives in bursts.
//
// Interleaved assigns group g the packets g, g+groupCount, g+2*groupCount, ... so adjacent
// packets belong to different groups. The same single parity and the same parity overhead then
// recover any burst up to groupCount long. Burst tolerance becomes the group count instead of 1.
//
// Both layouts are kept because the choice is negotiated: a receiver using a different layout
// from the sender would XOR a packet out of the wrong group and hand plausible garbage to the
// decoder rather than failing cleanly.
struct GroupLayout
{
    uint32_t totalDataPackets = 0;
    bool interleaved = false;

    uint32_t Count() const { return GroupCount(totalDataPackets); }

    // Which group owns a given data packet.
    uint32_t GroupOf(uint32_t packetIndex) const
    {
        const uint32_t count = Count();
        if (count == 0)
        {
            return 0;
        }
        return interleaved ? (packetIndex % count) : (packetIndex / protocol::FEC_GROUP_SIZE);
    }

    // How many data packets a group owns.
    uint32_t MemberCount(uint32_t groupIndex) const
    {
        if (groupIndex >= Count())
        {
            return 0;
        }
        if (!interleaved)
        {
            const uint32_t start = groupIndex * protocol::FEC_GROUP_SIZE;
            return std::min(start + protocol::FEC_GROUP_SIZE, totalDataPackets) - start;
        }
        const uint32_t stride = Count();
        return (totalDataPackets - groupIndex + stride - 1) / stride;
    }

    // The k-th data packet of a group, in ascending packet order.
    uint32_t Member(uint32_t groupIndex, uint32_t k) const
    {
        return interleaved ? (groupIndex + k * Count())
                           : (groupIndex * protocol::FEC_GROUP_SIZE + k);
    }
};

} // namespace fec
} // namespace oxr
