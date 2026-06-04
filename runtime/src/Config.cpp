// SPDX-License-Identifier: MPL-2.0

#include "Config.h"
#include "RuntimePlatform.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>

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
                if (val > 0 && val <= 200)
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
            "OXRSys: Reloaded config from {} (runtime_enabled={} bitrate={}Mbps fov={} res_scale={:.2f} keyframe={}s preset={} transport={} quest_logcat={})",
            configFilePath,
            newValues.runtimeEnabled,
            newValues.bitrateMbps,
            newValues.fovDegrees,
            newValues.resolutionScale,
            newValues.keyframeIntervalSec,
            newValues.encoderPreset,
            newValues.streamingTransport,
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
    spdlog::info("  bitrate={}Mbps fov={}° res_scale={:.2f} keyframe={}s preset={} transport={}",
                  values_.bitrateMbps, values_.fovDegrees, values_.resolutionScale,
                  values_.keyframeIntervalSec, values_.encoderPreset, values_.streamingTransport);
}

// ─── Quest logcat capture ────────────────────────────────────────────────────

void Config::StartLogcatCapture()
{
#if defined(_WIN32)
    spdlog::warn("Quest logcat capture is disabled on Windows in this runtime build");
    return;
#else
    if (logcatRunning_ || logcatProcess_ != nullptr)
    {
        return;
    }

    // Clear logcat first, then start capturing
    system("adb logcat -c 2>/dev/null");

    std::string cmd = "adb logcat -s "
                      "'OXRSys-Android:*' "
                      "'OXRSys-Network:*' "
                      "'OXRSys-Decoder:*' "
                      "2>/dev/null";

    logcatProcess_ = popen(cmd.c_str(), "r");
    if (logcatProcess_ == nullptr)
    {
        spdlog::warn("Failed to start adb logcat capture");
        return;
    }

    std::filesystem::create_directories(std::filesystem::path(questLogFilePath).parent_path());
    logcatFile_ = fopen(questLogFilePath.c_str(), "w");
    if (logcatFile_ == nullptr)
    {
        spdlog::warn("Failed to open {} for writing", questLogFilePath);
        pclose(logcatProcess_);
        logcatProcess_ = nullptr;
        return;
    }

    logcatRunning_ = true;

    // Background thread to pipe logcat → file
    std::thread([this]()
    {
        char buf[4096];
        while (logcatRunning_ && logcatProcess_ != nullptr)
        {
            if (fgets(buf, sizeof(buf), logcatProcess_) != nullptr)
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
    }).detach();

    spdlog::info("Quest logcat capture started → {}", questLogFilePath);
#endif
}

void Config::StopLogcatCapture()
{
    logcatRunning_ = false;

    if (logcatProcess_ != nullptr)
    {
#if defined(_WIN32)
        fclose(logcatProcess_);
#else
        pclose(logcatProcess_);
#endif
        logcatProcess_ = nullptr;
    }

    if (logcatFile_ != nullptr)
    {
        fclose(logcatFile_);
        logcatFile_ = nullptr;
    }
}
