// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "VideoEncoder.h"

#include <string>
#include <vector>

#include <oxrsys/protocol/Protocol.h>

namespace oxrsys::video_codec_selection
{

oxr::protocol::VideoCodec ParseConfiguredVideoCodec(const std::string& value);

bool ClientSupportsVideoCodec(const oxr::protocol::ClientConnect& clientConnect,
                              oxr::protocol::VideoCodec codec);

std::vector<oxr::protocol::VideoCodec> BuildCodecCandidates(
    const std::string& configuredVideoCodec,
    const oxr::protocol::ClientConnect& clientConnect,
    const VideoEncoder::BackendCapabilities& backendCapabilities);

} // namespace oxrsys::video_codec_selection
