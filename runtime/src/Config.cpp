// SPDX-License-Identifier: MPL-2.0

#include "Config.h"
#include "RuntimePlatform.h"

#include <oxrsys/protocol/Protocol.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
namespace
{

std::string ResolveAdbExecutable()
{
    auto executableFile = [](const std::string& path) {
        return !path.empty() && access(path.c_str(), X_OK) == 0;
    };

    if (const char* overridePath = std::getenv("OXRSYS_ADB_PATH");
        overridePath != nullptr && executableFile(overridePath))
    {
        return overridePath;
    }

    for (const char* envName : {"ANDROID_HOME", "ANDROID_SDK_ROOT"})
    {
        const char* sdkRoot = std::getenv(envName);
        if (sdkRoot == nullptr || *sdkRoot == '\0')
        {
            continue;
        }
        std::filesystem::path adbPath = std::filesystem::path(sdkRoot) / "platform-tools" / "adb";
        if (executableFile(adbPath.string()))
        {
            return adbPath.string();
        }
    }

    for (const char* adbPath : {
             "/opt/homebrew/bin/adb",
             "/usr/local/bin/adb",
             "/Applications/Android Studio.app/Contents/platform-tools/adb",
         })
    {
        if (executableFile(adbPath))
        {
            return adbPath;
        }
    }

    if (const char* pathEnv = std::getenv("PATH"))
    {
        std::string pathList = pathEnv;
        size_t start = 0;
        while (start <= pathList.size())
        {
            size_t end = pathList.find(':', start);
            if (end == std::string::npos)
            {
                end = pathList.size();
            }
            std::filesystem::path adbPath =
                std::filesystem::path(pathList.substr(start, end - start)) / "adb";
            if (executableFile(adbPath.string()))
            {
                return adbPath.string();
            }
            start = end + 1;
        }
    }

    return "";
}

void WaitForLogcatProcessExit(int pid)
{
    if (pid <= 0)
    {
        return;
    }

    for (int attempt = 0; attempt < 20; ++attempt)
    {
        int status = 0;
        const pid_t result = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (result == static_cast<pid_t>(pid) || result == -1)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    kill(static_cast<pid_t>(pid), SIGKILL);
    int status = 0;
    waitpid(static_cast<pid_t>(pid), &status, 0);
}

bool WaitForProcessExit(pid_t pid, std::chrono::milliseconds timeout, int* status)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const pid_t result = waitpid(pid, status, WNOHANG);
        if (result == pid)
        {
            return true;
        }
        if (result == -1)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

void TerminateProcessGroup(pid_t pid)
{
    if (pid <= 0)
    {
        return;
    }
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    int status = 0;
    if (!WaitForProcessExit(pid, std::chrono::milliseconds(500), &status))
    {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

bool RunAdbLogcatClear(const std::string& adbPath, std::chrono::milliseconds timeout)
{
    const pid_t pid = fork();
    if (pid < 0)
    {
        return false;
    }

    if (pid == 0)
    {
        setpgid(0, 0);

        const int nullFd = open("/dev/null", O_RDWR);
        if (nullFd >= 0)
        {
            dup2(nullFd, STDIN_FILENO);
            dup2(nullFd, STDOUT_FILENO);
            dup2(nullFd, STDERR_FILENO);
            close(nullFd);
        }

        execl(adbPath.c_str(),
              adbPath.c_str(),
              "logcat",
              "-c",
              static_cast<char*>(nullptr));
        _exit(127);
    }

    setpgid(pid, pid);
    int status = 0;
    if (!WaitForProcessExit(pid, timeout, &status))
    {
        TerminateProcessGroup(pid);
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace
#endif

Config& Config::Get()
{
    static Config instance;
    return instance;
}

Config::Config()
{
    DetectDylibDir();
    LoadConfigFile();
    SetupLogging();

    if (values_.questLogcat)
    {
        StartLogcatCapture();
    }
}

Config::~Config()
{
    Shutdown();
}

void Config::Shutdown()
{
    StopLogcatCapture();
}

// ─── Dylib directory detection ───────────────────────────────────────────────

void Config::DetectDylibDir()
{
    dylibDir = oxrsys::runtime_platform::ModuleDirectory(
        reinterpret_cast<const void*>(&Config::Get));
    appSupportDir = oxrsys::runtime_platform::ConfigRoot();
    std::string stateDir = oxrsys::runtime_platform::StateRoot();
    if (appSupportDir.empty())
    {
        appSupportDir = dylibDir;
    }
    if (stateDir.empty())
    {
        stateDir = appSupportDir;
    }

    configFilePath = appSupportDir + "/oxrsys-runtime.toml";
    logFilePath = stateDir + "/oxrsys-runtime.log";
    questLogFilePath = stateDir + "/oxrsys-headset.log";
    runtimeStatusPath = stateDir + "/runtime_status.json";
}

// ─── Config file parsing ─────────────────────────────────────────────────────

static std::string Trim(const std::string& s)
{
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        return "";
    }
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool ParseBool(const std::string& value)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == "true" || lower == "1" || lower == "yes";
}

static bool IsSupportedRefreshRate(uint32_t value)
{
    return value == 60 || value == 72 || value == 80 || value == 90 || value == 120;
}

static std::string ParseString(std::string value)
{
    value = Trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

ConfigValues ParseConfigToml(std::istream& input, const ConfigValues& defaults)
{
    ConfigValues values = defaults;
    std::string line;
    while (std::getline(input, line))
    {
        line = Trim(line);

        // Skip empty lines, comments, section headers
        if (line.empty() || line[0] == '#' || line[0] == '[')
        {
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }

        std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));

        try
        {
            if (key == "runtime_enabled")
            {
                values.runtimeEnabled = ParseBool(value);
            }
            else if (key == "file_logging")
            {
                values.fileLogging = ParseBool(value);
            }
            else if (key == "quest_logcat")
            {
                values.questLogcat = ParseBool(value);
            }
            else if (key == "bitrate_mbps")
            {
                int val = std::stoi(value);
                if (val >= static_cast<int>(oxr::protocol::STREAMING_MIN_BITRATE_MBPS) &&
                    val <= static_cast<int>(oxr::protocol::STREAMING_MAX_BITRATE_MBPS))
                {
                    values.bitrateMbps = val;
                }
            }
            else if (key == "fov_degrees")
            {
                int val = std::stoi(value);
                if (val >= 60 && val <= 150)
                {
                    values.fovDegrees = val;
                }
            }
            else if (key == "refresh_rate_hz")
            {
                int val = std::stoi(value);
                if (val > 0 && IsSupportedRefreshRate(static_cast<uint32_t>(val)))
                {
                    values.refreshRateHz = static_cast<uint32_t>(val);
                }
            }
            else if (key == "resolution_scale")
            {
                float val = std::stof(value);
                if (val >= 0.25f && val <= 1.0f)
                {
                    values.resolutionScale = val;
                }
            }
            else if (key == "keyframe_interval_sec")
            {
                int val = std::stoi(value);
                if (val >= 1 && val <= 10)
                {
                    values.keyframeIntervalSec = val;
                }
            }
            else if (key == "encoder_preset")
            {
                value = ParseString(value);
                if (value == "quality" || value == "balanced" || value == "speed")
                {
                    values.encoderPreset = value;
                }
            }
            else if (key == "transport")
            {
                value = ParseString(value);
                if (value == "auto" || value == "wifi" || value == "usb_adb")
                {
                    values.streamingTransport = value;
                }
            }
            else if (key == "foveated_encoding_preset")
            {
                value = ParseString(value);
                if (value == "off" || value == "light" || value == "medium" || value == "high")
                {
                    values.foveatedEncodingPreset = value;
                }
            }
            else if (key == "client_foveation_preset")
            {
                value = ParseString(value);
                if (value == "auto" || value == "off" || value == "light" ||
                    value == "medium" || value == "high")
                {
                    values.clientFoveationPreset = value;
                }
            }
            else if (key == "client_upscaling")
            {
                values.clientUpscaling = ParseBool(value);
            }
            else if (key == "client_reprojection")
            {
                value = ParseString(value);
                if (value == "off" || value == "pose" || value == "pose_warp")
                {
                    values.clientReprojectionMode = value;
                }
            }
            else if (key == "abr_mode")
            {
                value = ParseString(value);
                if (value == "off" || value == "bitrate" || value == "full")
                {
                    values.abrMode = value;
                }
            }
            else if (key == "headset_audio")
            {
                values.headsetAudio = ParseBool(value);
            }
        }
        catch (const std::exception&)
        {
            // Ignore malformed values and keep the last valid/default setting.
        }
    }

    return values;
}

bool Config::ResolveConfigFilePath(std::string& resolvedPath, bool& fileExists) const
{
    const std::string preferredConfigPath = appSupportDir + "/oxrsys-runtime.toml";
    const std::string legacyConfigPath = dylibDir + "/oxrsys-runtime.toml";
    if (std::filesystem::exists(preferredConfigPath))
    {
        resolvedPath = preferredConfigPath;
        fileExists = true;
        return true;
    }
    if (std::filesystem::exists(legacyConfigPath))
    {
        resolvedPath = legacyConfigPath;
        fileExists = true;
        return true;
    }

    resolvedPath = preferredConfigPath;
    fileExists = false;
    return true;
}

void Config::LoadConfigFile()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ReloadIfChangedLocked(true);
}

bool Config::ReloadIfChangedLocked(bool force)
{
    std::string resolvedPath;
    bool fileExists = false;
    ResolveConfigFilePath(resolvedPath, fileExists);

    std::filesystem::file_time_type writeTime = {};
    bool hasWriteTime = false;
    if (fileExists)
    {
        std::error_code ec;
        writeTime = std::filesystem::last_write_time(resolvedPath, ec);
        hasWriteTime = !ec;
    }

    const bool pathChanged = resolvedPath != configFilePath;
    const bool existenceChanged = fileExists != hasConfigFile_;
    const bool writeTimeChanged = fileExists &&
        (!hasKnownWriteTime_ || !hasWriteTime || writeTime != lastConfigWriteTime_);
    if (!force && !pathChanged && !existenceChanged && !writeTimeChanged)
    {
        return false;
    }

    ConfigValues oldValues = values_;
    ConfigValues newValues;
    if (fileExists)
    {
        std::ifstream file(resolvedPath);
        if (file.is_open())
        {
            newValues = ParseConfigToml(file);
        }
    }

    values_ = newValues;
    configFilePath = resolvedPath;
    hasConfigFile_ = fileExists;
    hasKnownWriteTime_ = fileExists && hasWriteTime;
    if (hasKnownWriteTime_)
    {
        lastConfigWriteTime_ = writeTime;
    }

    if (!force && oldValues.questLogcat != newValues.questLogcat)
    {
        if (newValues.questLogcat)
        {
            StartLogcatCapture();
        }
        else
        {
            StopLogcatCapture();
        }
    }

    if (!force)
    {
        spdlog::info(
            "OXRSys: Reloaded config from {} (runtime_enabled={} bitrate={}Mbps fov={} refresh={}Hz res_scale={:.2f} keyframe={}s preset={} transport={} ffe={} client_ffr={} upscaling={} reprojection={} abr={} audio={} quest_logcat={})",
            configFilePath,
            newValues.runtimeEnabled,
            newValues.bitrateMbps,
            newValues.fovDegrees,
            newValues.refreshRateHz,
            newValues.resolutionScale,
            newValues.keyframeIntervalSec,
            newValues.encoderPreset,
            newValues.streamingTransport,
            newValues.foveatedEncodingPreset,
            newValues.clientFoveationPreset,
            newValues.clientUpscaling,
            newValues.clientReprojectionMode,
            newValues.abrMode,
            newValues.headsetAudio,
            newValues.questLogcat);
    }

    return true;
}

void Config::RefreshIfNeeded()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    if (lastReloadCheck_ != std::chrono::steady_clock::time_point::min() &&
        now - lastReloadCheck_ < std::chrono::milliseconds(250))
    {
        return;
    }
    lastReloadCheck_ = now;
    ReloadIfChangedLocked(false);
}

ConfigValues Config::GetValues()
{
    RefreshIfNeeded();
    std::lock_guard<std::mutex> lock(mutex_);
    return values_;
}

// ─── Logging setup ───────────────────────────────────────────────────────────

void Config::SetupLogging()
{
    std::vector<spdlog::sink_ptr> sinks;

    // Console sink (stderr) — always active
    auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    consoleSink->set_level(spdlog::level::info);
    sinks.push_back(consoleSink);

    // File sink — optional, rotating 5MB with 2 backups
    if (values_.fileLogging)
    {
        try
        {
            std::filesystem::create_directories(std::filesystem::path(logFilePath).parent_path());
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                logFilePath, 5 * 1024 * 1024, 2);
            fileSink->set_level(spdlog::level::debug);
            sinks.push_back(fileSink);
        }
        catch (const spdlog::spdlog_ex& ex)
        {
            // Can't log yet, just skip file logging
            fprintf(stderr, "OXRSys: Failed to create log file %s: %s\n",
                    logFilePath.c_str(), ex.what());
        }
    }

    // Create the default logger with all sinks
    auto logger = std::make_shared<spdlog::logger>("", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));

    spdlog::info("OXRSys Runtime starting (config from {})", configFilePath);
    spdlog::info("  runtime_enabled={} file_logging={} quest_logcat={}",
                  values_.runtimeEnabled, values_.fileLogging, values_.questLogcat);
    spdlog::info("  bitrate={}Mbps fov={}° refresh={}Hz res_scale={:.2f} keyframe={}s preset={} transport={} ffe={} client_ffr={} upscaling={} reprojection={} abr={} audio={}",
                  values_.bitrateMbps, values_.fovDegrees, values_.refreshRateHz,
                  values_.resolutionScale, values_.keyframeIntervalSec,
                  values_.encoderPreset, values_.streamingTransport,
                  values_.foveatedEncodingPreset, values_.clientFoveationPreset,
                  values_.clientUpscaling, values_.clientReprojectionMode,
                  values_.abrMode, values_.headsetAudio);
}

// ─── Quest logcat capture ────────────────────────────────────────────────────

void Config::StartLogcatCapture()
{
#if defined(_WIN32)
    spdlog::warn("Quest logcat capture is disabled on Windows in this runtime build");
    return;
#else
    if (logcatRunning_.load() || logcatPipe_ != nullptr || logcatPid_ > 0)
    {
        return;
    }

    const std::string adbPath = ResolveAdbExecutable();
    if (adbPath.empty())
    {
        spdlog::warn("Quest logcat capture requested but adb was not found");
        return;
    }

    if (!RunAdbLogcatClear(adbPath, std::chrono::milliseconds(750)))
    {
        spdlog::warn("Quest logcat clear timed out or failed; continuing capture without clearing");
    }

    int pipeFds[2] = {-1, -1};
    if (pipe(pipeFds) != 0)
    {
        spdlog::warn("Failed to create pipe for adb logcat capture");
        return;
    }

    const pid_t pid = fork();
    if (pid < 0)
    {
        spdlog::warn("Failed to fork adb logcat capture");
        close(pipeFds[0]);
        close(pipeFds[1]);
        return;
    }

    if (pid == 0)
    {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        close(pipeFds[1]);

        const int nullFd = open("/dev/null", O_WRONLY);
        if (nullFd >= 0)
        {
            dup2(nullFd, STDERR_FILENO);
            close(nullFd);
        }

        execl(adbPath.c_str(),
              adbPath.c_str(),
              "logcat",
              "-s",
              "OXRSys-Android:*",
              "OXRSys-Network:*",
              "OXRSys-Decoder:*",
              static_cast<char*>(nullptr));
        _exit(127);
    }

    close(pipeFds[1]);
    logcatPipe_ = fdopen(pipeFds[0], "r");
    if (logcatPipe_ == nullptr)
    {
        spdlog::warn("Failed to open adb logcat capture pipe");
        close(pipeFds[0]);
        kill(pid, SIGTERM);
        WaitForLogcatProcessExit(static_cast<int>(pid));
        return;
    }

    std::filesystem::create_directories(std::filesystem::path(questLogFilePath).parent_path());
    logcatFile_ = fopen(questLogFilePath.c_str(), "w");
    if (logcatFile_ == nullptr)
    {
        spdlog::warn("Failed to open {} for writing", questLogFilePath);
        fclose(logcatPipe_);
        logcatPipe_ = nullptr;
        kill(pid, SIGTERM);
        WaitForLogcatProcessExit(static_cast<int>(pid));
        return;
    }

    logcatPid_ = static_cast<int>(pid);
    logcatRunning_.store(true);

    // Background thread to pipe logcat → file
    logcatThread_ = std::thread([this]()
    {
        char buf[4096];
        while (logcatRunning_.load() && logcatPipe_ != nullptr)
        {
            if (fgets(buf, sizeof(buf), logcatPipe_) != nullptr)
            {
                if (logcatFile_ != nullptr)
                {
                    fputs(buf, logcatFile_);
                    fflush(logcatFile_);
                }
            }
            else
            {
                break;  // EOF or error
            }
        }
    });

    spdlog::info("Quest logcat capture started via {} → {}", adbPath, questLogFilePath);
#endif
}

void Config::StopLogcatCapture()
{
    logcatRunning_.store(false);

#if defined(_WIN32)
    if (logcatPipe_ != nullptr)
    {
        fclose(logcatPipe_);
        logcatPipe_ = nullptr;
    }
#else
    if (logcatPid_ > 0)
    {
        kill(static_cast<pid_t>(logcatPid_), SIGTERM);
        WaitForLogcatProcessExit(logcatPid_);
        logcatPid_ = -1;
    }
#endif

    if (logcatThread_.joinable())
    {
        logcatThread_.join();
    }

    if (logcatPipe_ != nullptr)
    {
        fclose(logcatPipe_);
        logcatPipe_ = nullptr;
    }

    if (logcatFile_ != nullptr)
    {
        fclose(logcatFile_);
        logcatFile_ = nullptr;
    }
}
