// SPDX-License-Identifier: MPL-2.0

#include "RuntimePlatform.h"

#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
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

} // namespace oxrsys::runtime_platform
