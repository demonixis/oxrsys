// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <chrono>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <istream>
#include <mutex>
#include <string>
#include <thread>

struct ConfigValues
{
    bool runtimeEnabled = true;     // Allow this runtime to accept xrCreateInstance
    uint32_t bitrateMbps = 50;      // H.265 encoding bitrate in Mbps
    uint32_t fovDegrees = 100;      // Legacy fallback FOV when a client omits eyeFov
    uint32_t refreshRateHz = 72;    // Preferred headset refresh rate
    float resolutionScale = 0.75f;  // Encode resolution multiplier (0.25-1.0)
    uint32_t keyframeIntervalSec = 2; // Seconds between forced keyframes
    std::string encoderPreset = "balanced"; // "quality", "balanced", "speed"
    std::string streamingTransport = "auto"; // "auto", "wifi", "usb_adb"
    std::string foveatedEncodingPreset = "off"; // "off", "light", "medium", "high"
    std::string clientFoveationPreset = "auto"; // "auto", "off", "light", "medium", "high"
    bool clientUpscaling = false;    // Enable Quest shader upscaling
    bool headsetAudio = false;       // Stream server audio to the headset

    bool fileLogging = true;        // Write logs to oxrsys-runtime.log
    bool questLogcat = false;       // Capture Quest logcat to oxrsys-headset.log
};

ConfigValues ParseConfigToml(std::istream& input, const ConfigValues& defaults = {});

/**
 * Runtime configuration loaded from the platform config directory
 * (macOS: ~/Library/Application Support/OXRSys, Linux: XDG_CONFIG_HOME/oxrsys)
 * with a fallback to the library-local config file.
 *
 * Singleton initialized once on first access. Configures spdlog sinks
 * (console + optional rotating file) and optionally captures Quest logcat.
 *
 * Dynamic fields are reloaded opportunistically when the config file changes.
 * Logging sink changes still require a restart.
 */
class Config
{
public:
    static Config& Get();

    ConfigValues GetValues();
    void RefreshIfNeeded();

    // Resolved paths
    std::string appSupportDir;      // Platform config directory
    std::string dylibDir;           // Directory containing the runtime library
    std::string configFilePath;     // Resolved config file path
    std::string logFilePath;        // Full path to oxrsys-runtime.log
    std::string questLogFilePath;   // Full path to oxrsys-headset.log
    std::string runtimeStatusPath;  // Full path to runtime_status.json

    void Shutdown();

private:
    Config();
    ~Config();

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    void DetectDylibDir();
    void LoadConfigFile();
    void SetupLogging();
    bool ResolveConfigFilePath(std::string& resolvedPath, bool& fileExists) const;
    bool ReloadIfChangedLocked(bool force);
    void StartLogcatCapture();
    void StopLogcatCapture();

    mutable std::mutex mutex_;
    ConfigValues values_;
    std::filesystem::file_time_type lastConfigWriteTime_ = {};
    bool hasConfigFile_ = false;
    bool hasKnownWriteTime_ = false;
    std::chrono::steady_clock::time_point lastReloadCheck_ = std::chrono::steady_clock::time_point::min();
    FILE* logcatPipe_ = nullptr;
    FILE* logcatFile_ = nullptr;
    std::atomic_bool logcatRunning_{false};
    std::thread logcatThread_;
    int logcatPid_ = -1;
};
