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

// True when this process is an x86_64 binary translated by Rosetta on Apple Silicon.
// VideoToolbox HEVC hardware encode is unavailable under Rosetta, so the runtime
// falls back to H.264 when this is true (see PreferredVideoCodec).
bool RunningUnderRosetta();

// macOS marketing major version (e.g. 27 on macOS 27.x), 0 if unknown or not Apple.
// VideoToolbox's internal RGB->YCbCr conversion under Rosetta emits all-zero
// chroma before macOS 27, which matters now that the encoder feeds BGRA directly.
int MacOSMajorVersion();

} // namespace oxrsys::runtime_platform
