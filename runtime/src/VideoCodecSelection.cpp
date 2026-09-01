// SPDX-License-Identifier: MPL-2.0

#include "VideoCodecSelection.h"

#include <algorithm>

namespace oxrsys::video_codec_selection
{
namespace
{

uint32_t VideoCodecCapabilityFlag(oxr::protocol::VideoCodec codec)
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

bool IsValidStreamingCodec(oxr::protocol::VideoCodec codec)
{
    return codec == oxr::protocol::VideoCodec::H265 ||
           codec == oxr::protocol::VideoCodec::H264;
}

} // namespace

oxr::protocol::VideoCodec ParseConfiguredVideoCodec(const std::string& value)
{
    if (value == "h264")
    {
        return oxr::protocol::VideoCodec::H264;
    }
    return oxr::protocol::VideoCodec::H265;
}

bool ClientSupportsVideoCodec(const oxr::protocol::ClientConnect& clientConnect,
                              oxr::protocol::VideoCodec codec)
{
    if (!IsValidStreamingCodec(codec))
    {
        return false;
    }
    if (clientConnect.supportedCodecs == 0)
    {
        return codec == oxr::protocol::VideoCodec::H265;
    }
    return (clientConnect.supportedCodecs & VideoCodecCapabilityFlag(codec)) != 0;
}

std::vector<oxr::protocol::VideoCodec> BuildCodecCandidates(
    const std::string& configuredVideoCodec,
    const oxr::protocol::ClientConnect& clientConnect,
    const VideoEncoder::BackendCapabilities& backendCapabilities)
{
    std::vector<oxr::protocol::VideoCodec> candidates;
    auto addCandidate = [&](oxr::protocol::VideoCodec codec) {
        if (!IsValidStreamingCodec(codec) ||
            !backendCapabilities.SupportsCodec(codec) ||
            !ClientSupportsVideoCodec(clientConnect, codec) ||
            std::find(candidates.begin(), candidates.end(), codec) != candidates.end())
        {
            return;
        }
        candidates.push_back(codec);
    };

    if (configuredVideoCodec != "auto")
    {
        addCandidate(ParseConfiguredVideoCodec(configuredVideoCodec));
    }

    addCandidate(static_cast<oxr::protocol::VideoCodec>(clientConnect.preferredCodec));
    addCandidate(oxr::protocol::VideoCodec::H265);
    addCandidate(oxr::protocol::VideoCodec::H264);
    return candidates;
}

} // namespace oxrsys::video_codec_selection
