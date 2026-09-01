// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "VideoCodecSelection.h"

using oxr::protocol::CLIENT_CODEC_CAPABILITY_H264;
using oxr::protocol::CLIENT_CODEC_CAPABILITY_H265;
using oxr::protocol::ClientConnect;
using oxr::protocol::VideoCodec;

namespace
{

VideoEncoder::BackendCapabilities Backend(bool h264, bool h265)
{
    VideoEncoder::BackendCapabilities capabilities;
    capabilities.backendName = "test";
    capabilities.hardwareEncoder = h264 || h265;
    capabilities.supportsH264 = h264;
    capabilities.supportsH265 = h265;
    return capabilities;
}

ClientConnect Client(uint32_t supportedCodecs, VideoCodec preferredCodec)
{
    ClientConnect client = {};
    client.supportedCodecs = supportedCodecs;
    client.preferredCodec = static_cast<uint32_t>(preferredCodec);
    return client;
}

} // namespace

TEST_CASE("Codec selection follows config, client preference, H.265, then H.264", "[video][codec]")
{
    const auto capabilities = Backend(true, true);
    const auto client = Client(CLIENT_CODEC_CAPABILITY_H264 | CLIENT_CODEC_CAPABILITY_H265,
                               VideoCodec::H264);

    auto candidates = oxrsys::video_codec_selection::BuildCodecCandidates(
        "h265", client, capabilities);
    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0] == VideoCodec::H265);
    CHECK(candidates[1] == VideoCodec::H264);

    candidates = oxrsys::video_codec_selection::BuildCodecCandidates(
        "auto", client, capabilities);
    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0] == VideoCodec::H264);
    CHECK(candidates[1] == VideoCodec::H265);
}

TEST_CASE("Codec selection treats legacy clients as H.265-only", "[video][codec]")
{
    const auto candidates = oxrsys::video_codec_selection::BuildCodecCandidates(
        "auto", Client(0, VideoCodec::H264), Backend(true, true));
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0] == VideoCodec::H265);
}

TEST_CASE("Codec selection falls back when the backend lacks H.265", "[video][codec]")
{
    const auto candidates = oxrsys::video_codec_selection::BuildCodecCandidates(
        "h265",
        Client(CLIENT_CODEC_CAPABILITY_H264 | CLIENT_CODEC_CAPABILITY_H265, VideoCodec::H265),
        Backend(true, false));
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0] == VideoCodec::H264);
}

TEST_CASE("Codec selection returns no candidates when no shared hardware codec exists", "[video][codec]")
{
    const auto candidates = oxrsys::video_codec_selection::BuildCodecCandidates(
        "auto",
        Client(CLIENT_CODEC_CAPABILITY_H264, VideoCodec::H264),
        Backend(false, true));
    CHECK(candidates.empty());
}
