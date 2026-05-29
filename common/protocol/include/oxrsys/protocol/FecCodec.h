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

// Compute the data packet range [outStart, outEnd) for a given FEC group index.
inline void GroupRange(uint32_t groupIndex, uint32_t totalDataPackets,
                       uint32_t& outStart, uint32_t& outEnd)
{
    outStart = groupIndex * protocol::FEC_GROUP_SIZE;
    outEnd = std::min(outStart + protocol::FEC_GROUP_SIZE, totalDataPackets);
}

} // namespace fec
} // namespace oxr
