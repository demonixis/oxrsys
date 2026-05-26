// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "Config.h"

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
keyframe_interval_sec = 4
encoder_preset = "quality"

[logging]
file_logging = false
quest_logcat = yes
)TOML");

    const ConfigValues values = ParseConfigToml(input);

    CHECK(values.runtimeEnabled == false);
    CHECK(values.bitrateMbps == 85);
    CHECK(values.fovDegrees == 100);
    CHECK(values.resolutionScale == 0.8f);
    CHECK(values.keyframeIntervalSec == 4);
    CHECK(values.encoderPreset == "quality");
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
keyframe_interval_sec = 0
encoder_preset = "turbo"
)TOML");

    ConfigValues defaults;
    defaults.runtimeEnabled = false;
    defaults.bitrateMbps = 64;
    defaults.resolutionScale = 0.5f;
    defaults.keyframeIntervalSec = 3;
    defaults.encoderPreset = "speed";

    const ConfigValues values = ParseConfigToml(input, defaults);

    CHECK(values.runtimeEnabled == false);
    CHECK(values.bitrateMbps == 64);
    CHECK(values.resolutionScale == 0.5f);
    CHECK(values.keyframeIntervalSec == 3);
    CHECK(values.encoderPreset == "speed");
}

TEST_CASE("Config parser accepts streaming transport", "[config]")
{
    std::istringstream input(R"TOML(
[streaming]
transport = "usb_adb"
)TOML");

    const ConfigValues values = ParseConfigToml(input);

    CHECK(values.streamingTransport == "usb_adb");
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
