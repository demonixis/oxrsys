// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

#include <oxrsys/protocol/Protocol.h>
#include "RuntimePlatform.h"

namespace oxrsys
{

// The codec an oxrsys runtime prefers on this machine. HEVC hardware encode is
// unavailable under Rosetta, so an x86_64-translated runtime (e.g. when loaded
// by wineopenxr.so for a Wine D3D11 app) uses H.264.
inline oxr::protocol::VideoCodec PreferredVideoCodecFor(bool underRosetta)
{
    return underRosetta ? oxr::protocol::VideoCodec::H264
                        : oxr::protocol::VideoCodec::H265;
}

// The video codec the runtime should encode + advertise in stream headers.
inline oxr::protocol::VideoCodec PreferredVideoCodec()
{
    // Translation status is fixed for the process lifetime; resolve the sysctl once
    // (this is queried several times per frame from the streaming hot path).
    static const oxr::protocol::VideoCodec codec =
        PreferredVideoCodecFor(runtime_platform::RunningUnderRosetta());
    return codec;
}

inline uint32_t VideoCodecCapabilityFlag(oxr::protocol::VideoCodec codec)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
            return oxr::protocol::CLIENT_CODEC_CAPABILITY_H264;
        case oxr::protocol::VideoCodec::AV1:
            return oxr::protocol::CLIENT_CODEC_CAPABILITY_AV1;
        case oxr::protocol::VideoCodec::H265:
        default:
            return oxr::protocol::CLIENT_CODEC_CAPABILITY_H265;
    }
}

// VideoToolbox exposes no HEVC hardware encode under Rosetta, so a translated
// runtime can only deliver H.264.
inline bool RuntimeSupportsVideoCodec(oxr::protocol::VideoCodec codec, bool underRosetta)
{
    if (underRosetta)
    {
        return codec == oxr::protocol::VideoCodec::H264;
    }
    return codec == oxr::protocol::VideoCodec::H265 ||
           codec == oxr::protocol::VideoCodec::H264;
}

inline bool ClientSupportsVideoCodec(const oxr::protocol::ClientConnect& clientConnect,
                                     oxr::protocol::VideoCodec codec)
{
    if (clientConnect.supportedCodecs == 0)
    {
        return codec == oxr::protocol::VideoCodec::H265;
    }
    return (clientConnect.supportedCodecs & VideoCodecCapabilityFlag(codec)) != 0;
}

inline oxr::protocol::VideoCodec ParseConfiguredVideoCodec(const std::string& value)
{
    if (value == "h264")
    {
        return oxr::protocol::VideoCodec::H264;
    }
    return oxr::protocol::VideoCodec::H265;
}

// Negotiate the stream codec: the configured/client-preferred codec when both
// sides support it, otherwise the best codec in the intersection of what the
// runtime can encode and what the client advertises. When that intersection is
// empty (an H.265-only or legacy client on a Rosetta runtime), the runtime
// cannot encode outside its own set, so this returns its preferred codec;
// callers must detect the mismatch via ClientSupportsVideoCodec and surface it.
inline oxr::protocol::VideoCodec SelectVideoCodec(const std::string& configuredCodec,
                                                  const oxr::protocol::ClientConnect& clientConnect,
                                                  bool underRosetta)
{
    const auto negotiable = [&clientConnect, underRosetta](oxr::protocol::VideoCodec codec) {
        return RuntimeSupportsVideoCodec(codec, underRosetta) &&
               ClientSupportsVideoCodec(clientConnect, codec);
    };
    if (configuredCodec == "auto")
    {
        const auto preferred = static_cast<oxr::protocol::VideoCodec>(clientConnect.preferredCodec);
        if (negotiable(preferred))
        {
            return preferred;
        }
    }
    else
    {
        const oxr::protocol::VideoCodec requested = ParseConfiguredVideoCodec(configuredCodec);
        if (negotiable(requested))
        {
            return requested;
        }
    }
    if (negotiable(oxr::protocol::VideoCodec::H265))
    {
        return oxr::protocol::VideoCodec::H265;
    }
    if (negotiable(oxr::protocol::VideoCodec::H264))
    {
        return oxr::protocol::VideoCodec::H264;
    }
    return PreferredVideoCodecFor(underRosetta);
}

} // namespace oxrsys
