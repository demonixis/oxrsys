// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <oxrsys/protocol/Protocol.h>
#include "RuntimePlatform.h"

namespace oxrsys
{

// The video codec the runtime should encode + advertise in stream headers.
// HEVC hardware encode is unavailable under Rosetta, so an x86_64-translated
// runtime (e.g. when loaded by wineopenxr.so for a Wine D3D11 app) uses H.264.
inline oxr::protocol::VideoCodec PreferredVideoCodec()
{
    return runtime_platform::RunningUnderRosetta() ? oxr::protocol::VideoCodec::H264
                                                   : oxr::protocol::VideoCodec::H265;
}

} // namespace oxrsys
