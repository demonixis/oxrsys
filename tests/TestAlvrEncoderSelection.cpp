// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "AlvrEncoderSelection.h"

using oxr::protocol::VideoCodec;
using oxrsys::alvr::DecideEncoderTransport;
using oxrsys::alvr::EncoderIdentity;
using oxrsys::alvr::EncoderTransportMode;
using oxrsys::alvr::TransportDecision;

TEST_CASE("Transport decision: inproc config always selects in-process", "[encoder-selection]")
{
    for (const bool present : {false, true})
    {
        for (const uint32_t failures : {0u, 1u, 2u, 5u})
        {
            for (const bool budget : {false, true})
            {
                for (const bool healthy : {false, true})
                {
                    CHECK(DecideEncoderTransport("inproc", present, failures, budget, healthy) ==
                          TransportDecision::InProcess);
                }
            }
        }
    }
}

TEST_CASE("Transport decision: native never falls back to in-process", "[encoder-selection]")
{
    // Missing binary or exhausted respawn budget: retry later, no encoder.
    CHECK(DecideEncoderTransport("native", /*present=*/false, 0, true, false) ==
          TransportDecision::RetryLater);
    CHECK(DecideEncoderTransport("native", /*present=*/false, 3, false, false) ==
          TransportDecision::RetryLater);
    CHECK(DecideEncoderTransport("native", /*present=*/true, 1, /*budget=*/false, false) ==
          TransportDecision::RetryLater);
    // Budget elapsed (or no failure yet): try the helper, regardless of how
    // many failures accumulated — native mode never pins.
    CHECK(DecideEncoderTransport("native", true, 0, true, false) == TransportDecision::Native);
    CHECK(DecideEncoderTransport("native", true, 2, true, false) == TransportDecision::Native);
    CHECK(DecideEncoderTransport("native", true, 5, true, false) == TransportDecision::Native);
    // A currently healthy helper is never demoted by the budget.
    CHECK(DecideEncoderTransport("native", true, 1, false, /*healthy=*/true) ==
          TransportDecision::Native);
}

TEST_CASE("Transport decision: auto falls back and pins", "[encoder-selection]")
{
    // No helper binary: clean in-process fallback.
    CHECK(DecideEncoderTransport("auto", /*present=*/false, 0, true, false) ==
          TransportDecision::InProcess);
    // First use: native.
    CHECK(DecideEncoderTransport("auto", true, 0, true, false) == TransportDecision::Native);
    // One failure inside the 30s budget: temporary in-process fallback...
    CHECK(DecideEncoderTransport("auto", true, 1, /*budget=*/false, false) ==
          TransportDecision::InProcess);
    // ...then exactly one automatic respawn once the budget elapses.
    CHECK(DecideEncoderTransport("auto", true, 1, /*budget=*/true, false) ==
          TransportDecision::Native);
    // Two failures within one connected session: pinned until reconnect,
    // budget notwithstanding.
    CHECK(DecideEncoderTransport("auto", true, 2, true, false) ==
          TransportDecision::InProcessPinned);
    CHECK(DecideEncoderTransport("auto", true, 3, false, false) ==
          TransportDecision::InProcessPinned);
    // A healthy running helper stays native (budget does not demote it)...
    CHECK(DecideEncoderTransport("auto", true, 1, false, /*healthy=*/true) ==
          TransportDecision::Native);
}

TEST_CASE("Encoder identity: equal only when every field matches", "[encoder-selection]")
{
    EncoderIdentity base;
    base.totalWidth = 3008;
    base.height = 1664;
    base.fps = 72;
    base.codec = VideoCodec::H265;
    base.bitDepth = 8;
    base.transport = EncoderTransportMode::NativeHelper;

    CHECK(base == base);
    EncoderIdentity same = base;
    CHECK_FALSE(base != same);

    // Each field flip must break the identity (forces an encoder rebuild).
    EncoderIdentity width = base;
    width.totalWidth = 2880;
    CHECK(base != width);

    EncoderIdentity height = base;
    height.height = 1600;
    CHECK(base != height);

    EncoderIdentity fps = base;
    fps.fps = 90;
    CHECK(base != fps);

    EncoderIdentity codec = base;
    codec.codec = VideoCodec::H264;
    CHECK(base != codec);

    EncoderIdentity depth = base;
    depth.bitDepth = 10;
    CHECK(base != depth);

    EncoderIdentity format = base;
    format.pixelFormat = 0x34323076; // 'v420'
    CHECK(base != format);

    EncoderIdentity transport = base;
    transport.transport = EncoderTransportMode::InProcess;
    CHECK(base != transport);
}
