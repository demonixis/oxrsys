// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>

namespace oxrsys::runtime_platform
{

enum class PlatformKind
{
    MacOS,
    Linux,
    Windows,
    Unknown,
};

struct EnvironmentPaths
{
    std::string home;
    std::string xdgConfigHome;
    std::string xdgStateHome;
    std::string appData;
};

PlatformKind CurrentPlatform();
EnvironmentPaths CurrentEnvironmentPaths();
std::string ConfigRootForPlatform(PlatformKind platform, const EnvironmentPaths& environment);
std::string StateRootForPlatform(PlatformKind platform, const EnvironmentPaths& environment);
std::string ConfigRoot();
std::string StateRoot();
std::string ModuleDirectory(const void* symbolAddress);
uint64_t ProcessId();

// Raise the calling thread's scheduling priority so time-sensitive per-frame
// work is not delayed behind default-priority threads. Best effort on every
// platform: failures just keep the default policy.
void SetCurrentThreadTimeSensitive();

/** Returns the steady clock now in nanoseconds since its epoch. */
int64_t SteadyNowNs();

/**
 * Promotes the calling thread to the platform realtime scheduling class.
 *
 * periodNs declares the expected wake cadence to the scheduler. The class
 * is a contract. Per wake computation must stay short or the scheduler
 * demotes the thread. Returns false when the platform offers no such
 * class or rejects the request.
 */
bool PromoteCurrentThreadToRealtime(int64_t periodNs);

/**
 * Blocks the calling thread until the steady clock reaches deadlineNs.
 *
 * Returns immediately for deadlines in the past. Wake precision follows
 * the calling thread's scheduling class. Plain threads are subject to
 * timer coalescing in the millisecond range on macOS.
 */
void WaitUntilSteadyNs(int64_t deadlineNs);

} // namespace oxrsys::runtime_platform
