// SPDX-License-Identifier: MPL-2.0

#pragma once

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

} // namespace oxrsys::runtime_platform
