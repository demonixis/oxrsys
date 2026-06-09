// SPDX-License-Identifier: MPL-2.0

#include "RuntimePlatform.h"

#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace
{

#if defined(_WIN32)
std::string Utf8FromWide(const wchar_t* value, int length)
{
    if (value == nullptr || length <= 0)
    {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }

    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, length, output.data(), size, nullptr, nullptr);
    return output;
}

std::string EnvironmentValue(const wchar_t* key)
{
    const DWORD required = GetEnvironmentVariableW(key, nullptr, 0);
    if (required == 0)
    {
        return {};
    }

    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(key, value.data(), required);
    if (written == 0)
    {
        return {};
    }
    return Utf8FromWide(value.data(), static_cast<int>(written));
}
#else
std::string EnvironmentValue(const char* key)
{
    const char* value = std::getenv(key);
    if (value == nullptr || value[0] == '\0')
    {
        return {};
    }
    return value;
}
#endif

std::string JoinPath(const std::string& root, const char* suffix)
{
    if (root.empty())
    {
        return {};
    }
    return (std::filesystem::path(root) / suffix).generic_string();
}

std::string DirectoryName(const std::string& path)
{
    if (path.empty())
    {
        return {};
    }
    const std::filesystem::path filesystemPath(path);
    const std::filesystem::path parent = filesystemPath.parent_path();
    return parent.empty() ? "." : parent.generic_string();
}

} // namespace

namespace oxrsys::runtime_platform
{

PlatformKind CurrentPlatform()
{
#if defined(_WIN32)
    return PlatformKind::Windows;
#elif defined(__APPLE__)
    return PlatformKind::MacOS;
#elif defined(__linux__)
    return PlatformKind::Linux;
#else
    return PlatformKind::Unknown;
#endif
}

PlatformKind CurrentPlatformKind()
{
    return CurrentPlatform();
}

EnvironmentPaths CurrentEnvironmentPaths()
{
    EnvironmentPaths environment = {};
#if defined(_WIN32)
    environment.appData = EnvironmentValue(L"APPDATA");
    environment.localAppData = EnvironmentValue(L"LOCALAPPDATA");
    environment.home = EnvironmentValue(L"USERPROFILE");
#else
    environment.home = EnvironmentValue("HOME");
    environment.xdgConfigHome = EnvironmentValue("XDG_CONFIG_HOME");
    environment.xdgStateHome = EnvironmentValue("XDG_STATE_HOME");
    environment.appData = EnvironmentValue("APPDATA");
    environment.localAppData = EnvironmentValue("LOCALAPPDATA");
#endif
    return environment;
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
            if (!environment.localAppData.empty())
            {
                return JoinPath(environment.localAppData, "OXRSys");
            }
            return JoinPath(environment.home, "AppData/Local/OXRSys");
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

std::string ModuleDirectory()
{
    return ModuleDirectory(reinterpret_cast<const void*>(
        static_cast<uint64_t (*)()>(&ProcessId)));
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
        std::wstring path(MAX_PATH, L'\0');
        DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        while (length == path.size())
        {
            path.resize(path.size() * 2);
            length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        }
        if (length > 0)
        {
            return DirectoryName(Utf8FromWide(path.data(), static_cast<int>(length)));
        }
    }
#else
    Dl_info info = {};
    if (dladdr(symbolAddress, &info) && info.dli_fname != nullptr)
    {
        return DirectoryName(info.dli_fname);
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
