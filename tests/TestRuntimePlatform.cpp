// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "RuntimePlatform.h"
#include "RuntimeSockets.h"

using oxrsys::runtime_platform::ConfigRootForPlatform;
using oxrsys::runtime_platform::EnvironmentPaths;
using oxrsys::runtime_platform::PlatformKind;
using oxrsys::runtime_platform::StateRootForPlatform;

TEST_CASE("Runtime platform resolves Windows config and state roots")
{
    EnvironmentPaths environment;
    environment.home = "C:/Users/Ada";
    environment.appData = "C:/Users/Ada/AppData/Roaming";
    environment.localAppData = "C:/Users/Ada/AppData/Local";

    REQUIRE(ConfigRootForPlatform(PlatformKind::Windows, environment) ==
            "C:/Users/Ada/AppData/Roaming/OXRSys");
    REQUIRE(StateRootForPlatform(PlatformKind::Windows, environment) ==
            "C:/Users/Ada/AppData/Local/OXRSys");
}

TEST_CASE("Runtime platform falls back to Windows profile paths")
{
    EnvironmentPaths environment;
    environment.home = "C:/Users/Ada";

    REQUIRE(ConfigRootForPlatform(PlatformKind::Windows, environment) ==
            "C:/Users/Ada/AppData/Roaming/OXRSys");
    REQUIRE(StateRootForPlatform(PlatformKind::Windows, environment) ==
            "C:/Users/Ada/AppData/Local/OXRSys");
}

TEST_CASE("Runtime socket invalid handle helpers are stable")
{
    auto socket = oxrsys::runtime_socket::InvalidSocket;
    REQUIRE_FALSE(oxrsys::runtime_socket::IsValid(socket));
    oxrsys::runtime_socket::Close(socket);
    REQUIRE_FALSE(oxrsys::runtime_socket::IsValid(socket));
}
