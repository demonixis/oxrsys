// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace oxrsys::runtime_platform
{

struct EnvironmentPaths
{
    std::string home;
};

EnvironmentPaths CurrentEnvironmentPaths();
std::string ConfigRootForEnvironment(const EnvironmentPaths& environment);
std::string StateRootForEnvironment(const EnvironmentPaths& environment);
std::string ConfigRoot();
std::string StateRoot();
std::string ModuleDirectory(const void* symbolAddress);
uint64_t ProcessId();

// Raise the calling thread's scheduling priority so time-sensitive per-frame
// work is not delayed behind default-priority threads. Best effort on every
// macOS: failures just keep the default policy.
void SetCurrentThreadTimeSensitive();

// Give the calling thread a Mach time-constraint (realtime) policy for work that repeats every
// period: computation = a third of the period, constraint = half of it, preemptible. A thread that
// overruns its declared computation is demoted by the scheduler, not stalled. Returns false if the
// kernel refused the policy (the thread keeps its old one).
bool SetCurrentThreadRealtime(std::chrono::nanoseconds period);

// A thread's scheduling policy from before SetCurrentThreadRealtime, so it can be put back. Holds a
// Mach send right to the thread, released by RestoreThreadPolicy.
struct SavedThreadPolicy
{
    uint32_t thread = 0;
    bool timeConstraint = false; // false: the thread was on the standard policy
    uint32_t period = 0;
    uint32_t computation = 0;
    uint32_t constraint = 0;
    bool preemptible = false;
};

SavedThreadPolicy SaveCurrentThreadPolicy();

// Put a saved policy back. Any thread may call it; the saved thread need not be the caller. Returns
// false if the kernel refused it, for example because the thread has exited.
bool RestoreThreadPolicy(const SavedThreadPolicy& saved);

} // namespace oxrsys::runtime_platform
