// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "RuntimePlatform.h"
#include "RuntimeSockets.h"

TEST_CASE("RuntimePlatform resolves config and state roots per platform", "[runtime-platform]")
{
    oxrsys::runtime_platform::EnvironmentPaths environment = {};
    environment.home = "/home/tester";
    environment.xdgConfigHome = "/tmp/xdg-config";
    environment.xdgStateHome = "/tmp/xdg-state";
    environment.appData = "C:/Users/tester/AppData/Roaming";

    CHECK(oxrsys::runtime_platform::ConfigRootForPlatform(
              oxrsys::runtime_platform::PlatformKind::MacOS,
              environment) == "/home/tester/Library/Application Support/OXRSys");
    CHECK(oxrsys::runtime_platform::StateRootForPlatform(
              oxrsys::runtime_platform::PlatformKind::MacOS,
              environment) == "/home/tester/Library/Application Support/OXRSys");

    CHECK(oxrsys::runtime_platform::ConfigRootForPlatform(
              oxrsys::runtime_platform::PlatformKind::Linux,
              environment) == "/tmp/xdg-config/oxrsys");
    CHECK(oxrsys::runtime_platform::StateRootForPlatform(
              oxrsys::runtime_platform::PlatformKind::Linux,
              environment) == "/tmp/xdg-state/oxrsys");

    CHECK(oxrsys::runtime_platform::ConfigRootForPlatform(
              oxrsys::runtime_platform::PlatformKind::Windows,
              environment) == "C:/Users/tester/AppData/Roaming/OXRSys");
    CHECK(oxrsys::runtime_platform::StateRootForPlatform(
              oxrsys::runtime_platform::PlatformKind::Windows,
              environment) == "C:/Users/tester/AppData/Roaming/OXRSys");
}

TEST_CASE("RuntimePlatform falls back to home for Linux and Windows roots", "[runtime-platform]")
{
    oxrsys::runtime_platform::EnvironmentPaths environment = {};
    environment.home = "/home/tester";

    CHECK(oxrsys::runtime_platform::ConfigRootForPlatform(
              oxrsys::runtime_platform::PlatformKind::Linux,
              environment) == "/home/tester/.config/oxrsys");
    CHECK(oxrsys::runtime_platform::StateRootForPlatform(
              oxrsys::runtime_platform::PlatformKind::Linux,
              environment) == "/home/tester/.local/state/oxrsys");
    CHECK(oxrsys::runtime_platform::ConfigRootForPlatform(
              oxrsys::runtime_platform::PlatformKind::Windows,
              environment) == "/home/tester/AppData/Roaming/OXRSys");
}

TEST_CASE("RuntimePlatform exposes a non-zero process id", "[runtime-platform]")
{
    CHECK(oxrsys::runtime_platform::ProcessId() > 0);
}

TEST_CASE("RuntimeSockets invalid handle stays invalid after close", "[runtime-sockets]")
{
    auto socket = oxrsys::runtime_socket::InvalidSocket;
    CHECK(!oxrsys::runtime_socket::IsValid(socket));

    oxrsys::runtime_socket::Close(socket);
    CHECK(!oxrsys::runtime_socket::IsValid(socket));
}
