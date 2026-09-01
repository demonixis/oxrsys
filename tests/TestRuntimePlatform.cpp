// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "RuntimePlatform.h"
#include "RuntimeSockets.h"

#include <thread>

#include <pthread/qos.h>

TEST_CASE("RuntimePlatform resolves macOS config and state roots", "[runtime-platform]")
{
    oxrsys::runtime_platform::EnvironmentPaths environment = {};
    environment.home = "/home/tester";

    CHECK(oxrsys::runtime_platform::ConfigRootForEnvironment(environment) ==
          "/home/tester/Library/Application Support/OXRSys");
    CHECK(oxrsys::runtime_platform::StateRootForEnvironment(environment) ==
          "/home/tester/Library/Application Support/OXRSys");
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

TEST_CASE("SetCurrentThreadTimeSensitive raises the calling thread's priority",
          "[runtime-platform]")
{
    // Catch2 assertions are not safe off the test thread; the worker records
    // what it saw and the checks run after the join.
    bool queried = false;
    bool stateOk = false;
    std::thread worker([&] {
        oxrsys::runtime_platform::SetCurrentThreadTimeSensitive();
        qos_class_t qosClass = QOS_CLASS_UNSPECIFIED;
        int relativePriority = 0;
        queried = pthread_get_qos_class_np(pthread_self(), &qosClass, &relativePriority) == 0;
        stateOk = qosClass == QOS_CLASS_USER_INTERACTIVE;
    });
    worker.join();
    CHECK(queried);
    CHECK(stateOk);
}
