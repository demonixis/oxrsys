// SPDX-License-Identifier: MPL-2.0
//
// FEC is a protocol property, not a client property: every client reimplements the grouping, so
// the behaviour has to be pinned somewhere that does not depend on any of them.

#include <catch2/catch_test_macros.hpp>

#include <oxrsys/protocol/FecCodec.h>
#include <oxrsys/protocol/Protocol.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

using namespace oxr;

namespace {

// A frame of deterministic data packets, one parity per group, with a given loss set applied.
// Returns true when every lost data packet is recoverable, i.e. no group lost more than one.
bool FrameRecovers(uint32_t totalPackets, bool interleaved, const std::set<uint32_t>& lost)
{
    const fec::GroupLayout layout{totalPackets, interleaved};
    for (uint32_t g = 0; g < layout.Count(); g++)
    {
        uint32_t missing = 0;
        for (uint32_t k = 0; k < layout.MemberCount(g); k++)
        {
            if (lost.count(layout.Member(g, k)) != 0)
            {
                missing++;
            }
        }
        if (missing > 1)
        {
            return false;
        }
    }
    return true;
}

std::set<uint32_t> Burst(uint32_t start, uint32_t length, uint32_t totalPackets)
{
    std::set<uint32_t> lost;
    for (uint32_t i = 0; i < length && start + i < totalPackets; i++)
    {
        lost.insert(start + i);
    }
    return lost;
}

} // namespace

TEST_CASE("XOR parity recovers one lost packet from a group", "[protocol][fec]")
{
    const uint32_t count = 5;
    const uint16_t P = protocol::MAX_PACKET_PAYLOAD;

    std::vector<std::vector<uint8_t>> data(count, std::vector<uint8_t>(P));
    std::vector<const uint8_t*> ptrs(count);
    std::vector<uint16_t> sizes(count, P);
    for (uint32_t i = 0; i < count; i++)
    {
        for (uint16_t j = 0; j < P; j++)
        {
            data[i][j] = static_cast<uint8_t>((i * 37 + j * 5 + 11) & 0xFF);
        }
        ptrs[i] = data[i].data();
    }

    std::vector<uint8_t> parity(P);
    fec::Encode(ptrs.data(), sizes.data(), count, parity.data());

    for (uint32_t dropped = 0; dropped < count; dropped++)
    {
        std::vector<const uint8_t*> present;
        std::vector<uint16_t> presentSizes;
        for (uint32_t i = 0; i < count; i++)
        {
            if (i == dropped) continue;
            present.push_back(ptrs[i]);
            presentSizes.push_back(sizes[i]);
        }
        std::vector<uint8_t> recovered(P);
        fec::Decode(present.data(), presentSizes.data(),
                    static_cast<uint32_t>(present.size()), parity.data(), recovered.data());
        REQUIRE(memcmp(recovered.data(), data[dropped].data(), P) == 0);
    }
}

TEST_CASE("Both group layouts partition the frame exactly once", "[protocol][fec]")
{
    for (uint32_t total : {1u, 9u, 10u, 11u, 25u, 100u, 250u, 251u})
    {
        for (bool interleaved : {false, true})
        {
            const fec::GroupLayout layout{total, interleaved};
            std::set<uint32_t> seen;
            for (uint32_t g = 0; g < layout.Count(); g++)
            {
                for (uint32_t k = 0; k < layout.MemberCount(g); k++)
                {
                    const uint32_t idx = layout.Member(g, k);
                    REQUIRE(idx < total);
                    REQUIRE(layout.GroupOf(idx) == g);
                    REQUIRE(seen.insert(idx).second);
                }
            }
            REQUIRE(seen.size() == total);

            // Receivers gather a group into a fixed array sized FEC_GROUP_SIZE. Interleaving
            // must not make any group larger than that, or they overflow.
            for (uint32_t g = 0; g < layout.Count(); g++)
            {
                REQUIRE(layout.MemberCount(g) <= protocol::FEC_GROUP_SIZE);
            }
        }
    }
}

TEST_CASE("Interleaving spreads adjacent packets across groups", "[protocol][fec]")
{
    const uint32_t total = 250;
    const fec::GroupLayout contiguous{total, false};
    const fec::GroupLayout interleaved{total, true};

    // Contiguous: neighbours share a group, so one parity cannot cover both.
    REQUIRE(contiguous.GroupOf(0) == contiguous.GroupOf(1));

    // Interleaved: neighbours are always in different groups.
    for (uint32_t i = 0; i + 1 < total; i++)
    {
        REQUIRE(interleaved.GroupOf(i) != interleaved.GroupOf(i + 1));
    }
}

TEST_CASE("A burst is byte-exactly recovered through interleaved group membership",
          "[protocol][fec]")
{
    // The layout tests above only count losses per group. This one actually runs the codec
    // through interleaved indices and compares bytes, so a wrong member mapping cannot pass by
    // being merely well-shaped.
    const uint32_t total = 64;
    const uint16_t P = protocol::MAX_PACKET_PAYLOAD;
    const fec::GroupLayout layout{total, true};

    std::vector<std::vector<uint8_t>> data(total, std::vector<uint8_t>(P));
    for (uint32_t i = 0; i < total; i++)
    {
        for (uint16_t j = 0; j < P; j++)
        {
            data[i][j] = static_cast<uint8_t>((i * 131 + j * 17 + 7) & 0xFF);
        }
    }

    // Parity per interleaved group, exactly as the server emits it.
    std::vector<std::vector<uint8_t>> parity(layout.Count(), std::vector<uint8_t>(P));
    for (uint32_t g = 0; g < layout.Count(); g++)
    {
        const uint32_t members = layout.MemberCount(g);
        std::vector<const uint8_t*> ptrs(members);
        std::vector<uint16_t> sizes(members, P);
        for (uint32_t k = 0; k < members; k++)
        {
            ptrs[k] = data[layout.Member(g, k)].data();
        }
        fec::Encode(ptrs.data(), sizes.data(), members, parity[g].data());
    }

    // Drop a burst the contiguous layout could not have survived.
    const uint32_t burstStart = 11;
    const uint32_t burstLen = 4;
    const std::set<uint32_t> lost = Burst(burstStart, burstLen, total);
    REQUIRE_FALSE(FrameRecovers(total, false, lost));
    REQUIRE(FrameRecovers(total, true, lost));

    // Recover each lost packet from its own group and compare against the original bytes.
    for (uint32_t missing : lost)
    {
        const uint32_t g = layout.GroupOf(missing);
        const uint32_t members = layout.MemberCount(g);
        std::vector<const uint8_t*> present;
        std::vector<uint16_t> presentSizes;
        for (uint32_t k = 0; k < members; k++)
        {
            const uint32_t idx = layout.Member(g, k);
            if (idx == missing) continue;
            REQUIRE(lost.count(idx) == 0);  // only one loss per group, or this is not recoverable
            present.push_back(data[idx].data());
            presentSizes.push_back(P);
        }
        std::vector<uint8_t> recovered(P);
        fec::Decode(present.data(), presentSizes.data(),
                    static_cast<uint32_t>(present.size()), parity[g].data(), recovered.data());
        REQUIRE(memcmp(recovered.data(), data[missing].data(), P) == 0);
    }
}

TEST_CASE("Interleaving recovers adjacent bursts that the contiguous layout loses",
          "[protocol][fec]")
{
    const uint32_t total = 250;                       // a keyframe-sized frame
    const uint32_t groups = fec::GroupCount(total);   // 25 groups, so bursts up to 25

    // A two-packet burst is the common Wi-Fi case and the contiguous layout cannot survive it
    // anywhere except across a group boundary.
    REQUIRE_FALSE(FrameRecovers(total, false, Burst(0, 2, total)));
    REQUIRE(FrameRecovers(total, true, Burst(0, 2, total)));

    // Interleaved recovers any burst up to the group count, at identical parity overhead.
    for (uint32_t len = 1; len <= groups; len++)
    {
        REQUIRE(FrameRecovers(total, true, Burst(7, len, total)));
    }

    // One packet beyond the group count wraps onto an already-hit group and is unrecoverable.
    REQUIRE_FALSE(FrameRecovers(total, true, Burst(7, groups + 1, total)));

    // Neither layout is hurt by scattered single losses spaced beyond the stride.
    std::set<uint32_t> scattered;
    for (uint32_t i = 0; i < total; i += groups + 3)
    {
        scattered.insert(i);
    }
    REQUIRE(FrameRecovers(total, true, scattered));
}
