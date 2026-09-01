// SPDX-License-Identifier: MPL-2.0

#include "RuntimePlatform.h"

#include <cstdlib>
#include <filesystem>

#include <dlfcn.h>
#include <pthread/qos.h>
#include <unistd.h>

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

EnvironmentPaths CurrentEnvironmentPaths()
{
    EnvironmentPaths paths = {};
    paths.home = GetEnvironment("HOME");
    return paths;
}

std::string ConfigRootForEnvironment(const EnvironmentPaths& environment)
{
    return JoinPath(environment.home, "Library/Application Support/OXRSys");
}

std::string StateRootForEnvironment(const EnvironmentPaths& environment)
{
    return ConfigRootForEnvironment(environment);
}

std::string ConfigRoot()
{
    return ConfigRootForEnvironment(CurrentEnvironmentPaths());
}

std::string StateRoot()
{
    return StateRootForEnvironment(CurrentEnvironmentPaths());
}

std::string ModuleDirectory(const void* symbolAddress)
{
    Dl_info info = {};
    if (dladdr(symbolAddress, &info) && info.dli_fname != nullptr)
    {
        return std::filesystem::path(info.dli_fname).parent_path().string();
    }
    return ".";
}

uint64_t ProcessId()
{
    return static_cast<uint64_t>(getpid());
}

void SetCurrentThreadTimeSensitive()
{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

} // namespace oxrsys::runtime_platform
