// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "StreamingReconfigure.h"
#include "StreamingTransportPolicy.h"

using namespace oxrsys::streaming_reconfigure;

TEST_CASE("Stream reconfiguration is allowed only on reliable control transport", "[streaming][reconfigure]")
{
    CHECK(AllowsLiveReconfigure(true, true));
    CHECK_FALSE(AllowsLiveReconfigure(false, true));
    CHECK_FALSE(AllowsLiveReconfigure(true, false));
    CHECK_FALSE(AllowsLiveReconfigure(false, false));
}

TEST_CASE("Pending stream reconfiguration retries and times out", "[streaming][reconfigure]")
{
    PendingState pending;
    pending.Begin(7, 1000);

    CHECK(pending.state() == State::Pending);
    CHECK(pending.sequence() == 7);
    CHECK(pending.Tick(1200, 500, 2) == TickAction::None);
    CHECK(pending.Tick(1500, 500, 2) == TickAction::Retry);
    CHECK(pending.retryCount() == 1);
    CHECK(pending.Tick(2000, 500, 2) == TickAction::Retry);
    CHECK(pending.retryCount() == 2);
    CHECK(pending.Tick(2500, 500, 2) == TickAction::Timeout);
    CHECK(pending.state() == State::TimedOut);
    CHECK(pending.sequence() == 0);
}

TEST_CASE("Pending stream reconfiguration accepts only the matching ack", "[streaming][reconfigure]")
{
    PendingState pending;
    pending.Begin(3, 1000);

    CHECK_FALSE(pending.AcceptAck(2, true));
    CHECK(pending.state() == State::Pending);
    CHECK_FALSE(pending.AcceptAck(3, false));
    CHECK(pending.state() == State::Idle);

    pending.Begin(4, 2000);
    CHECK(pending.AcceptAck(4, true));
    CHECK(pending.state() == State::Acked);
    pending.CompleteAcked();
    CHECK(pending.state() == State::Idle);
}

TEST_CASE("Reserved spatial TCP listener is optional without a spatial backend", "[streaming][transport]")
{
    using oxrsys::streaming_transport::UsbAdbTcpListenersReady;

    CHECK(UsbAdbTcpListenersReady(true, true, true, false, false));
    CHECK(UsbAdbTcpListenersReady(true, true, true, true, false));
    CHECK_FALSE(UsbAdbTcpListenersReady(false, true, true, true, false));
    CHECK_FALSE(UsbAdbTcpListenersReady(true, false, true, true, false));
    CHECK_FALSE(UsbAdbTcpListenersReady(true, true, false, true, false));
    CHECK_FALSE(UsbAdbTcpListenersReady(true, true, true, false, true));
}
