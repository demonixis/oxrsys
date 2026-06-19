// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "Config.h"

#include <chrono>
#include <sstream>

TEST_CASE("Config parser accepts quoted presets and keeps defaults for invalid values", "[config]")
{
    std::istringstream input(R"TOML(
[general]
runtime_enabled = false

[streaming]
bitrate_mbps = 85
fov_degrees = 30
resolution_scale = 0.8
refresh_rate_hz = 120
keyframe_interval_sec = 4
encoder_preset = "quality"
foveated_encoding_preset = "medium"
client_foveation_preset = "high"
client_upscaling = true
client_reprojection = "pose_warp"
abr_mode = "full"
headset_audio = true

[logging]
file_logging = false
quest_logcat = yes
)TOML");

    const ConfigValues values = ParseConfigToml(input);

    CHECK(values.runtimeEnabled == false);
    CHECK(values.bitrateMbps == 85);
    CHECK(values.fovDegrees == 100);
    CHECK(values.resolutionScale == 0.8f);
    CHECK(values.refreshRateHz == 120);
    CHECK(values.keyframeIntervalSec == 4);
    CHECK(values.encoderPreset == "quality");
    CHECK(values.foveatedEncodingPreset == "medium");
    CHECK(values.clientFoveationPreset == "high");
    CHECK(values.clientUpscaling == true);
    CHECK(values.clientReprojectionMode == "pose_warp");
    CHECK(values.abrMode == "full");
    CHECK(values.headsetAudio == true);
    CHECK(values.streamingTransport == "auto");
    CHECK(values.fileLogging == false);
    CHECK(values.questLogcat == true);
}

TEST_CASE("Config parser preserves provided defaults when values are malformed", "[config]")
{
    std::istringstream input(R"TOML(
[streaming]
bitrate_mbps = nope
resolution_scale = 2.0
refresh_rate_hz = 144
keyframe_interval_sec = 0
encoder_preset = "turbo"
foveated_encoding_preset = "extreme"
client_foveation_preset = "ultra"
client_reprojection = "warp_all_the_time"
abr_mode = "turbo"
)TOML");

    ConfigValues defaults;
    defaults.runtimeEnabled = false;
    defaults.bitrateMbps = 64;
    defaults.resolutionScale = 0.5f;
    defaults.refreshRateHz = 80;
    defaults.keyframeIntervalSec = 3;
    defaults.encoderPreset = "speed";
    defaults.foveatedEncodingPreset = "light";
    defaults.clientFoveationPreset = "medium";
    defaults.clientUpscaling = true;
    defaults.clientReprojectionMode = "pose";
    defaults.abrMode = "bitrate";

    const ConfigValues values = ParseConfigToml(input, defaults);

    CHECK(values.runtimeEnabled == false);
    CHECK(values.bitrateMbps == 64);
    CHECK(values.resolutionScale == 0.5f);
    CHECK(values.refreshRateHz == 80);
    CHECK(values.keyframeIntervalSec == 3);
    CHECK(values.encoderPreset == "speed");
    CHECK(values.foveatedEncodingPreset == "light");
    CHECK(values.clientFoveationPreset == "medium");
    CHECK(values.clientUpscaling == true);
    CHECK(values.clientReprojectionMode == "pose");
    CHECK(values.abrMode == "bitrate");
}

TEST_CASE("Config parser accepts streaming transport", "[config]")
{
    std::istringstream input(R"TOML(
[streaming]
transport = "usb_adb"
client_foveation_preset = "auto"
)TOML");

    const ConfigValues values = ParseConfigToml(input);

    CHECK(values.streamingTransport == "usb_adb");
    CHECK(values.clientFoveationPreset == "auto");
}

TEST_CASE("Config parser enables Quest logcat capture from TOML", "[config]")
{
    std::istringstream input(R"TOML(
[logging]
quest_logcat = true
)TOML");

    const ConfigValues values = ParseConfigToml(input);

    CHECK(values.questLogcat == true);
}

TEST_CASE("Config singleton initializes with bounded Quest logcat clear", "[config][logcat]")
{
    const auto start = std::chrono::steady_clock::now();
    ConfigValues values = Config::Get().GetValues();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed < std::chrono::seconds(5));
    CHECK(values.bitrateMbps >= 1);

    Config::Get().Shutdown();
}
