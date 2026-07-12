// SPDX-License-Identifier: MPL-2.0

#include "RuntimePlatform.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace oxrsys::runtime_platform
{

namespace
{

std::string GetEnvironment(const char* name)
{
    const char* value = std::getenv(name);
    if (value != nullptr && value[0] != '\0')
    {
        return value;
    }
    return {};
}

std::string JoinPath(const std::string& root, const char* suffix)
{
    if (root.empty())
    {
        return {};
    }
    return (std::filesystem::path(root) / suffix).generic_string();
}

} // namespace

PlatformKind CurrentPlatform()
{
#if defined(__APPLE__)
    return PlatformKind::MacOS;
#elif defined(_WIN32)
    return PlatformKind::Windows;
#elif defined(__linux__)
    return PlatformKind::Linux;
#else
    return PlatformKind::Unknown;
#endif
}

EnvironmentPaths CurrentEnvironmentPaths()
{
    EnvironmentPaths paths = {};
    paths.home = GetEnvironment("HOME");
    paths.xdgConfigHome = GetEnvironment("XDG_CONFIG_HOME");
    paths.xdgStateHome = GetEnvironment("XDG_STATE_HOME");
    paths.appData = GetEnvironment("APPDATA");
    return paths;
}

std::string ConfigRootForPlatform(PlatformKind platform, const EnvironmentPaths& environment)
{
    switch (platform)
    {
        case PlatformKind::MacOS:
            return JoinPath(environment.home, "Library/Application Support/OXRSys");
        case PlatformKind::Linux:
            if (!environment.xdgConfigHome.empty())
            {
                return JoinPath(environment.xdgConfigHome, "oxrsys");
            }
            return JoinPath(environment.home, ".config/oxrsys");
        case PlatformKind::Windows:
            if (!environment.appData.empty())
            {
                return JoinPath(environment.appData, "OXRSys");
            }
            return JoinPath(environment.home, "AppData/Roaming/OXRSys");
        case PlatformKind::Unknown:
            return {};
    }
    return {};
}

std::string StateRootForPlatform(PlatformKind platform, const EnvironmentPaths& environment)
{
    switch (platform)
    {
        case PlatformKind::MacOS:
            return JoinPath(environment.home, "Library/Application Support/OXRSys");
        case PlatformKind::Linux:
            if (!environment.xdgStateHome.empty())
            {
                return JoinPath(environment.xdgStateHome, "oxrsys");
            }
            return JoinPath(environment.home, ".local/state/oxrsys");
        case PlatformKind::Windows:
            return ConfigRootForPlatform(platform, environment);
        case PlatformKind::Unknown:
            return {};
    }
    return {};
}

std::string ConfigRoot()
{
    return ConfigRootForPlatform(CurrentPlatform(), CurrentEnvironmentPaths());
}

std::string StateRoot()
{
    return StateRootForPlatform(CurrentPlatform(), CurrentEnvironmentPaths());
}

std::string ModuleDirectory(const void* symbolAddress)
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(symbolAddress),
                           &module))
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        if (length > 0)
        {
            return std::filesystem::path(path).parent_path().string();
        }
    }
#else
    Dl_info info = {};
    if (dladdr(symbolAddress, &info) && info.dli_fname != nullptr)
    {
        return std::filesystem::path(info.dli_fname).parent_path().string();
    }
#endif
    return ".";
}

uint64_t ProcessId()
{
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

void SetCurrentThreadTimeSensitive()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#elif defined(__linux__)
    // Realtime policies need CAP_SYS_NICE; unprivileged processes silently
    // keep SCHED_OTHER.
    sched_param param = {};
    param.sched_priority = sched_get_priority_min(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#endif
}

int64_t SteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#if defined(__APPLE__)

namespace
{

uint64_t NanosecondsToMachTicks(int64_t ns)
{
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info = {};
        mach_timebase_info(&info);

        return info;
    }();

    return static_cast<uint64_t>(ns) * timebase.denom / timebase.numer;
}

} // namespace

bool PromoteCurrentThreadToRealtime(int64_t periodNs)
{
    if (periodNs <= 0)
    {
        return false;
    }

    // The policy is a contract and the kernel demotes threads that overrun
    // it, so both budgets are loose. The constraint bounds wake to completion
    // latency, well above the observed worst case and capped at half a period
    // to keep the deadline clear of the next tick. The computation claims far
    // more than the microseconds of real tick work, so preemption hiccups do
    // not count as overruns
    const int64_t constraintNs = std::min<int64_t>(2'000'000, periodNs / 2);
    const int64_t computationNs = std::min<int64_t>(500'000, constraintNs / 2);

    thread_time_constraint_policy_data_t policy = {};

    policy.period = static_cast<uint32_t>(NanosecondsToMachTicks(periodNs));
    policy.computation = static_cast<uint32_t>(NanosecondsToMachTicks(computationNs));
    policy.constraint = static_cast<uint32_t>(NanosecondsToMachTicks(constraintNs));
    policy.preemptible = TRUE;

    const thread_port_t thread = mach_thread_self();

    const kern_return_t result = thread_policy_set(
        thread,
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    mach_port_deallocate(mach_task_self(), thread);

    return result == KERN_SUCCESS;
}

void WaitUntilSteadyNs(int64_t deadlineNs)
{
    const int64_t nowNs = SteadyNowNs();

    // The remaining duration is rebased onto the mach clock per call, which
    // keeps the deadline exact even if the steady clock epoch differs
    if (deadlineNs > nowNs)
    {
        mach_wait_until(mach_absolute_time() + NanosecondsToMachTicks(deadlineNs - nowNs));
    }
}

#else

bool PromoteCurrentThreadToRealtime(int64_t periodNs)
{
    (void)periodNs;

    return false;
}

void WaitUntilSteadyNs(int64_t deadlineNs)
{
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadlineNs)));
}

#endif

} // namespace oxrsys::runtime_platform
