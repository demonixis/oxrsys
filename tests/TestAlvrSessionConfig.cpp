// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "AlvrNalFraming.h"
#include "AlvrSessionConfig.h"

#include <cstdint>
#include <string>
#include <vector>

using oxrsys::alvr::AppendEncodedNal;
using oxrsys::alvr::ApplySessionSettings;
using oxrsys::alvr::MinimalSessionJson;
using oxrsys::alvr::ParseNegotiatedConfig;
using oxrsys::alvr::PendingEncodedFrame;

namespace
{

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Builds one Annex-B NAL: 4-byte start code, a header byte carrying the given
// nal type in the codec's bit position, then `payloadBytes` of filler.
std::vector<uint8_t> MakeNal(uint8_t nalType, bool h264, size_t payloadBytes = 3)
{
    const uint8_t header = h264 ? static_cast<uint8_t>(0x60u | (nalType & 0x1Fu)) // ref_idc | type
                                : static_cast<uint8_t>((nalType & 0x3Fu) << 1);   // type in bits 6..1
    std::vector<uint8_t> nal = {0x00, 0x00, 0x00, 0x01, header};
    nal.insert(nal.end(), payloadBytes, 0xAB);
    return nal;
}

} // namespace

TEST_CASE("MinimalSessionJson seeds auto-trust, bitrate, and absolute resolution", "[alvr-session]")
{
    const std::string json = MinimalSessionJson();
    CHECK(Contains(json, "\"auto_trust_clients\": true"));
    CHECK(Contains(json, "\"variant\": \"ConstantMbps\""));
    CHECK(Contains(json, "\"ConstantMbps\": 60"));
    // Absolute transcoding resolution, not a scale factor.
    CHECK(Contains(json, "\"variant\": \"Absolute\""));
    CHECK(Contains(json, "\"width\": 1512"));
    CHECK(Contains(json, "\"content\": 1680"));
}

TEST_CASE("ApplySessionSettings rewrites the bitrate seed from the toml", "[alvr-session]")
{
    const std::string synced = ApplySessionSettings(MinimalSessionJson(), 40);
    // Seed 60 -> 40, but the variant tag must survive (regex keyed on position).
    CHECK(Contains(synced, "\"ConstantMbps\": 40"));
    CHECK_FALSE(Contains(synced, "\"ConstantMbps\": 60"));
    CHECK(Contains(synced, "\"variant\": \"ConstantMbps\""));
    // max_buffering_frames is capped at 1.5 regardless of bitrate.
    CHECK(Contains(synced, "\"max_buffering_frames\": 1.5"));
}

TEST_CASE("ApplySessionSettings caps a large max_buffering_frames", "[alvr-session]")
{
    const std::string json = R"({ "video": { "max_buffering_frames": 4.0,
        "bitrate": { "mode": { "variant": "ConstantMbps", "ConstantMbps": 100 } } } })";
    const std::string synced = ApplySessionSettings(json, 30);
    CHECK(Contains(synced, "\"max_buffering_frames\": 1.5"));
    CHECK_FALSE(Contains(synced, "\"max_buffering_frames\": 4.0"));
    CHECK(Contains(synced, "\"ConstantMbps\": 30"));
}

TEST_CASE("ApplySessionSettings is idempotent", "[alvr-session]")
{
    const std::string once = ApplySessionSettings(MinimalSessionJson(), 40);
    const std::string twice = ApplySessionSettings(once, 40);
    // Re-applying the same bitrate must not perturb the already-synced document
    // (this equality is what SyncSessionSettings uses to skip rewriting the file).
    CHECK(twice == once);
}

TEST_CASE("ApplySessionSettings leaves a document with neither key unchanged", "[alvr-session]")
{
    const std::string json = R"({ "session_settings": { "connection": {} } })";
    CHECK(ApplySessionSettings(json, 55) == json);
}

TEST_CASE("ParseNegotiatedConfig extracts eye resolution and refresh rate", "[alvr-session]")
{
    // target_eye_resolution_width precedes eye_resolution_width to exercise the
    // leading-quote guard that keeps the shorter key from matching inside it.
    const std::string json = R"({ "openvr_config": {
        "target_eye_resolution_width": 9999,
        "eye_resolution_width": 1824,
        "eye_resolution_height": 1920,
        "refresh_rate": 90
    } })";
    const auto config = ParseNegotiatedConfig(json);
    CHECK(config.width == 1824);
    CHECK(config.height == 1920);
    CHECK(config.fps == 90);
}

TEST_CASE("ParseNegotiatedConfig reports zero for absent keys", "[alvr-session]")
{
    const auto config = ParseNegotiatedConfig(R"({ "openvr_config": {} })");
    CHECK(config.width == 0);
    CHECK(config.height == 0);
    CHECK(config.fps == 0);
}

TEST_CASE("AppendEncodedNal classifies H.264 NAL types", "[alvr-session]")
{
    PendingEncodedFrame frame;
    AppendEncodedNal(frame, MakeNal(7, /*h264=*/true).data(), MakeNal(7, true).size(), true, true);
    AppendEncodedNal(frame, MakeNal(8, /*h264=*/true).data(), MakeNal(8, true).size(), true, true);
    // SPS (7) and PPS (8) route to the config buffer, not the payload.
    CHECK(frame.config.size() == MakeNal(7, true).size() + MakeNal(8, true).size());
    CHECK(frame.data.empty());
    CHECK_FALSE(frame.isIdr);

    // IDR slice (type 5) is payload and latches isIdr.
    const auto idr = MakeNal(5, true);
    AppendEncodedNal(frame, idr.data(), idr.size(), true, true);
    CHECK(frame.data.size() == idr.size());
    CHECK(frame.isIdr);

    // Non-IDR slice (type 1) is payload but must not set isIdr on its own.
    PendingEncodedFrame nonIdr;
    const auto slice = MakeNal(1, true);
    AppendEncodedNal(nonIdr, slice.data(), slice.size(), false, true);
    CHECK(nonIdr.data.size() == slice.size());
    CHECK(nonIdr.config.empty());
    CHECK_FALSE(nonIdr.isIdr);
}

TEST_CASE("AppendEncodedNal classifies HEVC NAL types", "[alvr-session]")
{
    PendingEncodedFrame frame;
    // VPS (32), SPS (33), PPS (34) are parameter sets -> config buffer.
    for (uint8_t type : {32, 33, 34})
    {
        const auto nal = MakeNal(type, /*h264=*/false);
        AppendEncodedNal(frame, nal.data(), nal.size(), true, false);
    }
    CHECK(frame.config.size() == 3 * MakeNal(32, false).size());
    CHECK(frame.data.empty());
    CHECK_FALSE(frame.isIdr);

    // IDR_W_RADL (19) and IDR_N_LP (20) are payload and latch isIdr.
    for (uint8_t type : {19, 20})
    {
        const auto nal = MakeNal(type, false);
        AppendEncodedNal(frame, nal.data(), nal.size(), true, false);
    }
    CHECK(frame.data.size() == 2 * MakeNal(19, false).size());
    CHECK(frame.isIdr);
}

TEST_CASE("AppendEncodedNal ignores buffers too short to hold a header", "[alvr-session]")
{
    PendingEncodedFrame frame;
    const std::vector<uint8_t> tooShort = {0x00, 0x00, 0x00, 0x01}; // start code only
    AppendEncodedNal(frame, tooShort.data(), tooShort.size(), true, true);
    AppendEncodedNal(frame, nullptr, 0, true, true);
    CHECK(frame.config.empty());
    CHECK(frame.data.empty());
    CHECK_FALSE(frame.isIdr);
}

TEST_CASE("AppendEncodedNal aggregates config and payload in order", "[alvr-session]")
{
    PendingEncodedFrame frame;
    const auto sps = MakeNal(7, true, /*payloadBytes=*/1);
    const auto idr = MakeNal(5, true, /*payloadBytes=*/2);
    AppendEncodedNal(frame, sps.data(), sps.size(), true, true);
    AppendEncodedNal(frame, idr.data(), idr.size(), true, true);
    // config holds exactly the SPS bytes; data holds exactly the IDR bytes.
    CHECK(frame.config == std::vector<uint8_t>(sps.begin(), sps.end()));
    CHECK(frame.data == std::vector<uint8_t>(idr.begin(), idr.end()));
    CHECK(frame.isIdr);
}
