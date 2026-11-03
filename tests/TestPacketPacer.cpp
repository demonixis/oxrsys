// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "PacketPacer.h"

#include <cstdint>
#include <vector>

using oxrsys::streaming::PacingNowNs;
using oxrsys::streaming::PacingWindowNs;
using oxrsys::streaming::PacketPacer;
using oxrsys::streaming::PacketPacingConfig;

TEST_CASE("Packet pacing is off by default and for a zero window", "[pacing]")
{
    PacketPacingConfig config;
    CHECK(PacingWindowNs(config, false, 90) == 0);
    CHECK(PacingWindowNs(config, true, 90) == 0);

    PacketPacer pacer(1'000, 0, 50);
    CHECK_FALSE(pacer.Enabled());
    for (uint32_t i = 0; i < 50; ++i)
    {
        CHECK(pacer.DeadlineNs(i) == 1'000);
    }
}

TEST_CASE("Packet pacing window uses the keyframe window and caps at the frame period", "[pacing]")
{
    PacketPacingConfig config;
    config.windowMs = 3.0f;
    CHECK(PacingWindowNs(config, false, 90) == 3'000'000);
    // Keyframe window defaults to twice the frame window.
    CHECK(PacingWindowNs(config, true, 90) == 6'000'000);

    config.keyframeWindowMs = 7.5f;
    CHECK(PacingWindowNs(config, true, 90) == 7'500'000);

    // 0.8 of a 90 Hz period is 8.888 ms; 72 Hz is 11.111 ms.
    config.keyframeWindowMs = 20.0f;
    CHECK(PacingWindowNs(config, true, 90) == 8'888'888);
    CHECK(PacingWindowNs(config, true, 72) == 11'111'111);
    config.windowMs = 50.0f;
    CHECK(PacingWindowNs(config, false, 90) == 8'888'888);
}

TEST_CASE("Packet pacer spreads packets evenly and never past the window", "[pacing]")
{
    // A 70 kB frame: 50 data + 5 FEC packets over 4 ms.
    const int64_t start = 10'000'000;
    const int64_t window = 4'000'000;
    const uint32_t packets = 55;
    PacketPacer pacer(start, window, packets);
    REQUIRE(pacer.Enabled());
    CHECK(pacer.SpacingNs() == window / packets);

    CHECK(pacer.DeadlineNs(0) == start);
    int64_t previous = pacer.DeadlineNs(0);
    for (uint32_t i = 1; i < packets; ++i)
    {
        const int64_t deadline = pacer.DeadlineNs(i);
        const int64_t gap = deadline - previous;
        // Even: every gap is the spacing, give or take the integer remainder.
        CHECK(gap >= window / packets);
        CHECK(gap <= window / packets + 1);
        previous = deadline;
    }
    // The last packet is due one spacing before the window closes, never after.
    CHECK(pacer.DeadlineNs(packets - 1) < start + window);
    CHECK(pacer.DeadlineNs(packets - 1) >= start + window - window / packets - 1);
    // Indices past the plan clamp to the last deadline.
    CHECK(pacer.DeadlineNs(packets + 10) == pacer.DeadlineNs(packets - 1));
}

TEST_CASE("Packet pacer waits are never early and add no more than the window", "[pacing]")
{
    const int64_t window = 2'000'000;
    const uint32_t packets = 40;
    const int64_t start = PacingNowNs();
    PacketPacer pacer(start, window, packets);
    std::vector<int64_t> sendTimes;
    for (uint32_t i = 0; i < packets; ++i)
    {
        pacer.WaitForNextPacket();
        sendTimes.push_back(PacingNowNs());
    }
    for (uint32_t i = 0; i < packets; ++i)
    {
        CHECK(sendTimes[i] >= pacer.DeadlineNs(i));
    }
    // Loose bound for a loaded test machine: the frame finishes within the
    // window plus 2 ms of scheduling slack.
    CHECK(sendTimes.back() - start < window + 2'000'000);
    CHECK(pacer.PacketsSent() == packets);
}

TEST_CASE("Packet pacer flushes the rest as a burst when a newer frame is waiting", "[pacing]")
{
    const int64_t window = 50'000'000; // long, so an unflushed run would take 50 ms
    const uint32_t packets = 100;
    const int64_t start = PacingNowNs();
    PacketPacer pacer(start, window, packets);
    auto newerFrameAfterTenPackets = [&] { return pacer.PacketsSent() > 10; };
    for (uint32_t i = 0; i < packets; ++i)
    {
        pacer.WaitForNextPacket(newerFrameAfterTenPackets);
    }
    CHECK(pacer.Flushed());
    CHECK_FALSE(pacer.Enabled());
    // Ten packets were paced at 0.5 ms spacing; the other ninety went at once.
    CHECK(PacingNowNs() - start < 20'000'000);
}

TEST_CASE("Datagram count per NAL matches the sender's packetisation", "[pacing]")
{
    using oxrsys::streaming::DatagramsForNal;
    // The sender: ceil(size / 1400) data packets, one FEC parity after every 10 and after the tail.
    CHECK(DatagramsForNal(0, 1400, 10) == 0);
    CHECK(DatagramsForNal(1, 1400, 10) == 2);          // 1 data + 1 parity
    CHECK(DatagramsForNal(1400, 1400, 10) == 2);
    CHECK(DatagramsForNal(1401, 1400, 10) == 3);       // 2 data + 1 parity
    CHECK(DatagramsForNal(14000, 1400, 10) == 11);     // 10 data + 1 parity (a full group)
    CHECK(DatagramsForNal(14001, 1400, 10) == 13);     // 11 data + 2 parity (group + tail)
    CHECK(DatagramsForNal(300 * 1000, 1400, 10) == 215 + 22);   // a keyframe: 215 data, 22 parity
    CHECK(DatagramsForNal(1401, 1400, 0) == 2);        // no FEC: data only
}
