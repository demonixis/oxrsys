// SPDX-License-Identifier: MPL-2.0
// XOR-FEC round trip: encode a group's parity, drop one data packet, recover it.
// This is the recovery path NetworkReceiver relies on for lost video packets.
#include "test_util.h"

#include <oxrsys/protocol/Protocol.h>
#include <oxrsys/protocol/FecCodec.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace oxr;

void test_protocol_fec()
{
    SECTION("protocol: XOR-FEC recovers one lost packet per group");

    const uint32_t count = 5;
    const uint16_t P = protocol::MAX_PACKET_PAYLOAD;

    // Build `count` data packets with distinct, deterministic content.
    std::vector<std::vector<uint8_t>> data(count, std::vector<uint8_t>(P));
    std::vector<const uint8_t*> ptrs(count);
    std::vector<uint16_t> sizes(count, P);
    for (uint32_t i = 0; i < count; i++) {
        for (uint16_t j = 0; j < P; j++)
            data[i][j] = (uint8_t)((i * 37 + j * 5 + 11) & 0xFF);
        ptrs[i] = data[i].data();
    }

    // Parity over the whole group.
    std::vector<uint8_t> parity(P);
    fec::Encode(ptrs.data(), sizes.data(), count, parity.data());

    // Drop packet `missing`; recover from the others + parity.
    const uint32_t missing = 2;
    std::vector<const uint8_t*> present;
    std::vector<uint16_t> presentSizes;
    for (uint32_t i = 0; i < count; i++) {
        if (i == missing) continue;
        present.push_back(data[i].data());
        presentSizes.push_back(sizes[i]);
    }

    std::vector<uint8_t> recovered(P);
    fec::Decode(present.data(), presentSizes.data(), (uint32_t)present.size(),
                parity.data(), recovered.data());

    CHECK(memcmp(recovered.data(), data[missing].data(), P) == 0);

    // GroupCount / GroupRange sanity for a 23-packet frame (group size 10).
    CHECK(fec::GroupCount(23) == 3);
    uint32_t s = 0, e = 0;
    fec::GroupRange(2, 23, s, e);
    CHECK(s == 20 && e == 23);
}
