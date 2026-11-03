// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "StreamingFoveationSnapshot.h"
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

TEST_CASE("Eye-centred foveation needs the announce bit and the client capability",
          "[streaming][reconfigure][foveation]")
{
    CHECK(UsesFoveationEyeCenter(true, true));
    // An announce without a foveated layout, followed by a StreamConfigUpdate that turns
    // foveation on: the client uses the update's centre shift, so the encoder must too.
    CHECK_FALSE(UsesFoveationEyeCenter(false, true));
    CHECK_FALSE(UsesFoveationEyeCenter(true, false));
    CHECK_FALSE(UsesFoveationEyeCenter(false, false));
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

TEST_CASE("USB network clients are recognized by the link subnet", "[streaming][transport]")
{
    using oxrsys::streaming_transport::IsUsbNetworkClientAddress;

    CHECK(IsUsbNetworkClientAddress("192.168.42.2"));
    CHECK(IsUsbNetworkClientAddress("192.168.42.254"));
    CHECK_FALSE(IsUsbNetworkClientAddress("192.168.43.2"));
    CHECK_FALSE(IsUsbNetworkClientAddress("192.168.1.42"));
    CHECK_FALSE(IsUsbNetworkClientAddress("127.0.0.1"));
    CHECK_FALSE(IsUsbNetworkClientAddress(""));
    CHECK_FALSE(IsUsbNetworkClientAddress("not-an-ip"));
}

TEST_CASE("Stream foveation snapshot keeps the preset its encoded size was built for", "[streaming][foveation]")
{
    using namespace oxr::protocol;
    using oxrsys::streaming_foveation::TakeSnapshot;

    // Quest 3 eyes at Start with medium_plus: it travels as Medium with its own
    // parameters, and the encoded size comes from those parameters.
    const FoveationLayout mediumPlus =
        CalculateFoveationLayout(1512, 1680, FoveationPreset::Medium, MediumPlusParameters());
    const auto snapshot = TakeSnapshot(true, FoveationPreset::Medium, mediumPlus);
    CHECK(snapshot.preset == FoveationPreset::Medium);
    CHECK(snapshot.layout.optimizedEyeWidth == mediumPlus.optimizedEyeWidth);
    CHECK(snapshot.layout.optimizedEyeHeight == mediumPlus.optimizedEyeHeight);
    CHECK(snapshot.layout.eyeWidthRatio == mediumPlus.eyeWidthRatio);
    CHECK(snapshot.layout.eyeHeightRatio == mediumPlus.eyeHeightRatio);
    CHECK(snapshot.layout.parameters.centerSizeX == mediumPlus.parameters.centerSizeX);
    CHECK(snapshot.layout.parameters.centerSizeY == mediumPlus.parameters.centerSizeY);
    CHECK(snapshot.layout.parameters.edgeRatioX == mediumPlus.parameters.edgeRatioX);
    CHECK(snapshot.layout.parameters.edgeRatioY == mediumPlus.parameters.edgeRatioY);

    // Re-deriving from the preset enum would announce medium's parameters
    // against medium_plus dimensions.
    const FoveationLayout medium = CalculateFoveationLayout(1512, 1680, FoveationPreset::Medium);
    CHECK(medium.optimizedEyeHeight != snapshot.layout.optimizedEyeHeight);

    // An inactive stream announces and encodes Off at the target eye size, so
    // the encoder never sees enabled foveation with Off parameters.
    const FoveationLayout light = CalculateFoveationLayout(1512, 1680, FoveationPreset::Light);
    const auto inactive = TakeSnapshot(false, FoveationPreset::Light, light);
    CHECK(inactive.preset == FoveationPreset::Off);
    CHECK(inactive.layout.preset == FoveationPreset::Off);
    CHECK(inactive.layout.optimizedEyeWidth == 1512);
    CHECK(inactive.layout.optimizedEyeHeight == 1680);
    CHECK(inactive.layout.parameters.edgeRatioX == 1.0f);
    CHECK(inactive.layout.parameters.edgeRatioY == 1.0f);
}
