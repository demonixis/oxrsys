// SPDX-License-Identifier: MPL-2.0

#include "RuntimePlatform.h"

#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
    int size = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
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
    DWORD required = GetEnvironmentVariableW(key, nullptr, 0);
    if (required == 0)
    {
        return {};
    }
    std::wstring value(required, L'\0');
    DWORD written = GetEnvironmentVariableW(key, value.data(), required);
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

std::string DirectoryName(const std::string& path)
{
    if (path.empty())
    {
        return {};
    }
    std::filesystem::path filesystemPath(path);
    const std::filesystem::path parent = filesystemPath.parent_path();
    return parent.empty() ? "." : parent.string();
}

} // namespace

namespace oxrsys::runtime_platform
{

PlatformKind CurrentPlatformKind()
{
#if defined(_WIN32)
    return PlatformKind::Windows;
#elif defined(__APPLE__)
    return PlatformKind::MacOS;
#else
    return PlatformKind::Linux;
#endif
}

EnvironmentPaths CurrentEnvironmentPaths()
{
    EnvironmentPaths environment;
#if defined(_WIN32)
    environment.appData = EnvironmentValue(L"APPDATA");
    environment.localAppData = EnvironmentValue(L"LOCALAPPDATA");
    environment.home = EnvironmentValue(L"USERPROFILE");
#else
    environment.home = EnvironmentValue("HOME");
    environment.xdgConfigHome = EnvironmentValue("XDG_CONFIG_HOME");
    environment.xdgStateHome = EnvironmentValue("XDG_STATE_HOME");
#endif
    return environment;
}

std::string ConfigRootForPlatform(PlatformKind platform, const EnvironmentPaths& environment)
{
    switch (platform)
    {
        case PlatformKind::MacOS:
            if (!environment.home.empty())
            {
                return environment.home + "/Library/Application Support/OXRSys";
            }
            break;
        case PlatformKind::Linux:
            if (!environment.xdgConfigHome.empty())
            {
                return environment.xdgConfigHome + "/oxrsys";
            }
            if (!environment.home.empty())
            {
                return environment.home + "/.config/oxrsys";
            }
            break;
        case PlatformKind::Windows:
            if (!environment.appData.empty())
            {
                return environment.appData + "/OXRSys";
            }
            if (!environment.home.empty())
            {
                return environment.home + "/AppData/Roaming/OXRSys";
            }
            break;
    }
    return {};
}

std::string StateRootForPlatform(PlatformKind platform, const EnvironmentPaths& environment)
{
    switch (platform)
    {
        case PlatformKind::MacOS:
            if (!environment.home.empty())
            {
                return environment.home + "/Library/Application Support/OXRSys";
            }
            break;
        case PlatformKind::Linux:
            if (!environment.xdgStateHome.empty())
            {
                return environment.xdgStateHome + "/oxrsys";
            }
            if (!environment.home.empty())
            {
                return environment.home + "/.local/state/oxrsys";
            }
            break;
        case PlatformKind::Windows:
            if (!environment.localAppData.empty())
            {
                return environment.localAppData + "/OXRSys";
            }
            if (!environment.home.empty())
            {
                return environment.home + "/AppData/Local/OXRSys";
            }
            break;
    }
    return {};
}

std::string ModuleDirectory()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory),
                            &module))
    {
        return ".";
    }

    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size())
    {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    }
    if (length == 0)
    {
        return ".";
    }
    return DirectoryName(Utf8FromWide(path.data(), static_cast<int>(length)));
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&ModuleDirectory), &info) && info.dli_fname != nullptr)
    {
        return DirectoryName(info.dli_fname);
    }
    return ".";
#endif
}

std::string ConfigRoot()
{
    return ConfigRootForPlatform(CurrentPlatformKind(), CurrentEnvironmentPaths());
}

std::string StateRoot()
{
    return StateRootForPlatform(CurrentPlatformKind(), CurrentEnvironmentPaths());
}

int64_t ProcessId()
{
#if defined(_WIN32)
    return static_cast<int64_t>(GetCurrentProcessId());
#else
    return static_cast<int64_t>(getpid());
#endif
}

} // namespace oxrsys::runtime_platform
