// SPDX-License-Identifier: MPL-2.0

#include "AlvrSessionConfig.h"

#include <regex>

namespace oxrsys::alvr
{

const char* MinimalSessionJson()
{
    // Discovery with auto-trust (any v20 client on the LAN) plus the magic wired
    // hostname: server_core runs adb port-forwarding and launches the store
    // client itself when a device is plugged in. ALVR extrapolates all missing
    // settings against its defaults and rewrites the file with the full tree.
    static constexpr const char* kMinimalSessionJson = R"json({
  "client_connections": {
    "client.wired": {
      "display_name": "Quest (USB)",
      "current_ip": null,
      "manual_ips": [],
      "trusted": true,
      "connection_state": "Disconnected"
    }
  },
  "session_settings": {
    "connection": {
      "client_discovery": { "enabled": true, "content": { "auto_trust_clients": true } }
    },
    "video": {
      "bitrate": { "mode": { "variant": "ConstantMbps", "ConstantMbps": 60 } },
      "max_buffering_frames": 1.5,
      "transcoding_view_resolution": {
        "variant": "Absolute",
        "Absolute": { "width": 1512, "height": { "set": true, "content": 1680 } }
      },
      "foveated_encoding": { "enabled": false }
    },
    "audio": {
      "game_audio": {
        "enabled": true,
        "content": {
          "device": { "set": true, "content": { "variant": "NameSubstring", "NameSubstring": "BlackHole" } }
        }
      }
    }
  }
})json";
    return kMinimalSessionJson;
}

std::string ApplySessionSettings(const std::string& json, uint32_t bitrateMbps)
{
    // Targeted key rewrites instead of a JSON library: ALVR rewrites the file
    // with its full settings tree, so both keys exist after first run.
    // "variant": "ConstantMbps" does not match — the regex requires the key
    // position (quote before the colon).
    static const std::regex bitrateRe("\"ConstantMbps\"\\s*:\\s*[0-9.]+");
    static const std::regex bufferingRe("\"max_buffering_frames\"\\s*:\\s*[0-9.]+");

    std::string updated =
        std::regex_replace(json, bitrateRe, "\"ConstantMbps\": " + std::to_string(bitrateMbps));
    // Cap client-side frame queueing: larger values let server pacing drift pool
    // into standing latency before the vsync queue overflows into stutter.
    updated = std::regex_replace(updated, bufferingRe, "\"max_buffering_frames\": 1.5");
    return updated;
}

NegotiatedConfig ParseNegotiatedConfig(const std::string& json)
{
    static const std::regex widthRe("\"eye_resolution_width\"\\s*:\\s*(\\d+)");
    static const std::regex heightRe("\"eye_resolution_height\"\\s*:\\s*(\\d+)");
    static const std::regex fpsRe("\"refresh_rate\"\\s*:\\s*(\\d+)");

    NegotiatedConfig config;
    std::smatch match;
    if (std::regex_search(json, match, widthRe))
    {
        config.width = static_cast<uint32_t>(std::stoul(match[1]));
    }
    if (std::regex_search(json, match, heightRe))
    {
        config.height = static_cast<uint32_t>(std::stoul(match[1]));
    }
    if (std::regex_search(json, match, fpsRe))
    {
        config.fps = static_cast<uint32_t>(std::stoul(match[1]));
    }
    return config;
}

} // namespace oxrsys::alvr
