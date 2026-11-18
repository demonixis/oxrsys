// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "RuntimePlatform.h"
#include "RuntimeSockets.h"

#include <cerrno>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pthread/qos.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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

TEST_CASE("RuntimeSockets retires a socket but keeps its descriptor", "[runtime-sockets]")
{
    auto socket = oxrsys::runtime_socket::Create(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(oxrsys::runtime_socket::IsValid(socket));
    const int number = socket;

    CHECK(oxrsys::runtime_socket::RetireKeepingDescriptor(socket));
    CHECK(socket == number);
    CHECK(fcntl(socket, F_GETFD) != -1);
    int type = 0;
    socklen_t typeLength = sizeof(type);
    const bool notSocket =
        getsockopt(socket, SOL_SOCKET, SO_TYPE, &type, &typeLength) == -1 && errno == ENOTSOCK;
    CHECK(notSocket);

    oxrsys::runtime_socket::Close(socket);
    CHECK(!oxrsys::runtime_socket::IsValid(socket));
}

namespace
{

// True when a child started with posix_spawn, as Wine starts a game's child processes, still has
// this descriptor open.
bool ChildInheritsDescriptor(int descriptor)
{
    const std::string script = "test -e /dev/fd/" + std::to_string(descriptor);
    char shell[] = "/bin/sh";
    char flag[] = "-c";
    std::string scriptCopy = script;
    char* argv[] = {shell, flag, scriptCopy.data(), nullptr};
    pid_t pid = -1;
    if (posix_spawn(&pid, shell, nullptr, nullptr, argv, environ) != 0)
    {
        return false;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

TEST_CASE("RuntimeSockets sockets are not inherited by child processes", "[runtime-sockets]")
{
    auto socket = oxrsys::runtime_socket::Create(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(oxrsys::runtime_socket::IsValid(socket));
    // dup() clears close-on-exec: the copy shows the check sees an inherited descriptor.
    const int inheritable = dup(socket);
    REQUIRE(inheritable >= 0);

    CHECK(ChildInheritsDescriptor(inheritable));
    CHECK(!ChildInheritsDescriptor(socket));

    close(inheritable);
    oxrsys::runtime_socket::Close(socket);
}

TEST_CASE("RuntimeSockets accepted connections are not inherited by child processes",
          "[runtime-sockets]")
{
    auto listenSocket = oxrsys::runtime_socket::Create(AF_INET, SOCK_STREAM, 0);
    REQUIRE(oxrsys::runtime_socket::IsValid(listenSocket));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(listen(listenSocket, 1) == 0);
    socklen_t addressLength = sizeof(address);
    REQUIRE(getsockname(listenSocket, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0);

    auto client = oxrsys::runtime_socket::Create(AF_INET, SOCK_STREAM, 0);
    REQUIRE(oxrsys::runtime_socket::IsValid(client));
    REQUIRE(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    auto accepted = oxrsys::runtime_socket::Accept(listenSocket);
    REQUIRE(oxrsys::runtime_socket::IsValid(accepted));

    CHECK(!ChildInheritsDescriptor(accepted));

    oxrsys::runtime_socket::Close(accepted);
    oxrsys::runtime_socket::Close(client);
    oxrsys::runtime_socket::Close(listenSocket);
}

TEST_CASE("RuntimeSockets keeps the descriptor when /dev/null cannot be opened",
          "[runtime-sockets]")
{
    auto socket = oxrsys::runtime_socket::Create(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(oxrsys::runtime_socket::IsValid(socket));
    const int number = socket;
    rlimit original = {};
    REQUIRE(getrlimit(RLIMIT_NOFILE, &original) == 0);

    // Make open("/dev/null") fail with EMFILE: cap the descriptor table just above the socket
    // and fill every free number below it. No assertions run until the cap is lifted.
    rlimit capped = original;
    capped.rlim_cur = static_cast<rlim_t>(number) + 1;
    REQUIRE(setrlimit(RLIMIT_NOFILE, &capped) == 0);
    std::vector<int> fillers;
    for (int filler = dup(number); filler >= 0; filler = dup(number))
    {
        fillers.push_back(filler);
    }
    const bool tableFull = errno == EMFILE;
    const bool retired = oxrsys::runtime_socket::RetireKeepingDescriptor(socket);
    const bool restoredOk = setrlimit(RLIMIT_NOFILE, &original) == 0;

    // Every lower number is still taken, so a freed socket number would be the next one handed out.
    const int next = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int type = 0;
    socklen_t typeLength = sizeof(type);
    const bool stillSocket =
        getsockopt(number, SOL_SOCKET, SO_TYPE, &type, &typeLength) == 0 && type == SOCK_DGRAM;
    close(next);
    for (int filler : fillers)
    {
        close(filler);
    }

    REQUIRE(restoredOk);
    REQUIRE(tableFull);
    CHECK(!retired);
    CHECK(socket == number);
    CHECK(next != number);
    CHECK(stillSocket);

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
