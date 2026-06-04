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
};

struct EnvironmentPaths
{
    std::string home;
    std::string xdgConfigHome;
    std::string xdgStateHome;
    std::string appData;
    std::string localAppData;
};

PlatformKind CurrentPlatformKind();
EnvironmentPaths CurrentEnvironmentPaths();

std::string ConfigRootForPlatform(PlatformKind platform, const EnvironmentPaths& environment);
std::string StateRootForPlatform(PlatformKind platform, const EnvironmentPaths& environment);

std::string ModuleDirectory();
std::string ConfigRoot();
std::string StateRoot();
int64_t ProcessId();

} // namespace oxrsys::runtime_platform
