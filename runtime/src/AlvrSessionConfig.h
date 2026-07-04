// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>

// Pure session.json logic for the ALVR backend, factored out of
// AlvrStreamingBackend so the template, the toml->session.json rewrite, and the
// negotiated-config parse are unit-testable without the alvr dylib. These guard
// against ALVR schema drift (a renamed/moved key silently no-ops the rewrite).
namespace oxrsys::alvr
{

// The seed session.json written when none exists: client discovery + auto-trust
// + a wired client entry + video/audio defaults. ALVR extrapolates the rest and
// owns the file afterwards. The ConstantMbps value is only a seed;
// ApplySessionSettings rewrites it from the toml before alvr_initialize.
const char* MinimalSessionJson();

// Rewrites the bitrate (ConstantMbps) and caps max_buffering_frames in an
// existing session.json via targeted key regexes (no JSON library, matching the
// style of the negotiated-config parse). Returns the updated document; the
// result equals the input when both keys already hold the target values (the
// caller uses that equality to skip rewriting the file). A missing key is left
// untouched. Idempotent: applying the result again returns it unchanged.
std::string ApplySessionSettings(const std::string& json, uint32_t bitrateMbps);

// Post-handshake stream config server_core negotiated with the client, read from
// session.json's openvr_config. width/height/fps are 0 when their key is absent.
struct NegotiatedConfig
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
};

// Extracts eye_resolution_width/height and refresh_rate. The leading quote in
// each key regex keeps eye_resolution_width from matching inside
// target_eye_resolution_width.
NegotiatedConfig ParseNegotiatedConfig(const std::string& json);

} // namespace oxrsys::alvr
