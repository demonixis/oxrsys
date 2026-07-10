// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "CodecSelect.h"

using oxr::protocol::ClientConnect;
using oxr::protocol::VideoCodec;
using oxrsys::ClientSupportsVideoCodec;
using oxrsys::RuntimeSupportsVideoCodec;
using oxrsys::SelectVideoCodec;

namespace
{
ClientConnect MakeClient(uint32_t supportedCodecs, VideoCodec preferred = VideoCodec::H265)
{
    ClientConnect client{};
    client.supportedCodecs = supportedCodecs;
    client.preferredCodec = static_cast<uint32_t>(preferred);
    return client;
}

constexpr uint32_t kH265 = oxr::protocol::CLIENT_CODEC_CAPABILITY_H265;
constexpr uint32_t kH264 = oxr::protocol::CLIENT_CODEC_CAPABILITY_H264;
constexpr uint32_t kAv1 = oxr::protocol::CLIENT_CODEC_CAPABILITY_AV1;
} // namespace

TEST_CASE("Codec select: native runtime honors the client's preference", "[codec-select]")
{
    const auto client = MakeClient(kH265 | kH264, VideoCodec::H265);
    CHECK(SelectVideoCodec("auto", client, /*underRosetta=*/false) == VideoCodec::H265);
}

TEST_CASE("Codec select: Rosetta negotiates H.264 when the client supports it", "[codec-select]")
{
    // The client prefers H.265 but also decodes H.264; a Rosetta runtime can
    // only encode H.264, so negotiation lands there — within the client's set.
    const auto client = MakeClient(kH265 | kH264, VideoCodec::H265);
    const auto codec = SelectVideoCodec("auto", client, /*underRosetta=*/true);
    CHECK(codec == VideoCodec::H264);
    CHECK(ClientSupportsVideoCodec(client, codec));
}

TEST_CASE("Codec select: configured codec is gated by runtime support", "[codec-select]")
{
    const auto client = MakeClient(kH265 | kH264, VideoCodec::H265);
    CHECK(SelectVideoCodec("h265", client, /*underRosetta=*/false) == VideoCodec::H265);
    // The h265 request cannot be encoded under Rosetta; fall back inside the
    // client's advertised set instead of honoring the config.
    CHECK(SelectVideoCodec("h265", client, /*underRosetta=*/true) == VideoCodec::H264);
}

TEST_CASE("Codec select: legacy client keeps H.265 on a native runtime", "[codec-select]")
{
    const auto legacy = MakeClient(0);
    const auto codec = SelectVideoCodec("auto", legacy, /*underRosetta=*/false);
    CHECK(codec == VideoCodec::H265);
    CHECK(ClientSupportsVideoCodec(legacy, codec));
}

TEST_CASE("Codec select: legacy client on Rosetta gets H.264 flagged as unsupported",
          "[codec-select]")
{
    // A legacy client never advertised codecs; the runtime cannot encode H.265
    // under Rosetta, so it streams H.264 and the caller must warn — detectable
    // because the selection falls outside ClientSupportsVideoCodec.
    const auto legacy = MakeClient(0);
    const auto codec = SelectVideoCodec("auto", legacy, /*underRosetta=*/true);
    CHECK(codec == VideoCodec::H264);
    CHECK_FALSE(ClientSupportsVideoCodec(legacy, codec));
}

TEST_CASE("Codec select: explicit H.265-only client on Rosetta is a detectable mismatch",
          "[codec-select]")
{
    const auto client = MakeClient(kH265, VideoCodec::H265);
    const auto codec = SelectVideoCodec("auto", client, /*underRosetta=*/true);
    // No codec is in both sets; the runtime returns the only codec it can
    // encode, and the caller detects the contract violation to log it.
    CHECK(codec == VideoCodec::H264);
    CHECK_FALSE(ClientSupportsVideoCodec(client, codec));
}

TEST_CASE("Codec select: client without H.265 negotiates H.264 natively", "[codec-select]")
{
    const auto client = MakeClient(kH264, VideoCodec::H264);
    CHECK(SelectVideoCodec("auto", client, /*underRosetta=*/false) == VideoCodec::H264);
}

TEST_CASE("Codec select: AV1-only client falls back to the runtime preference", "[codec-select]")
{
    // The runtime never encodes AV1; with no common codec the historic H.265
    // fallback applies natively (and H.264 under Rosetta).
    const auto client = MakeClient(kAv1, VideoCodec::AV1);
    CHECK_FALSE(RuntimeSupportsVideoCodec(VideoCodec::AV1, /*underRosetta=*/false));
    CHECK(SelectVideoCodec("auto", client, /*underRosetta=*/false) == VideoCodec::H265);
    CHECK(SelectVideoCodec("auto", client, /*underRosetta=*/true) == VideoCodec::H264);
}
