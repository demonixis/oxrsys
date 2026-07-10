// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Annex-B NAL classification for the ALVR backend, factored out of
// AlvrStreamingBackend's encode-thread lambda so it is unit-testable without
// the alvr dylib or VideoToolbox. The HEVC branch can never run in the demo
// (H.264-only under Rosetta), so tests are the only coverage it has.
namespace oxrsys::alvr
{

// One encoded frame in flight: config NALs (SPS/PPS/VPS) split from the payload
// NALs, aggregated across per-NAL callbacks on the VideoToolbox thread.
struct PendingEncodedFrame
{
    uint64_t timestampNs = 0;
    bool isIdr = false;
    std::vector<uint8_t> config;
    std::vector<uint8_t> data;
};

// Classifies one Annex-B NAL (4-byte start code followed by the NAL header) and
// appends it to the frame: parameter sets (H.264 SPS/PPS types 7/8, HEVC
// VPS/SPS/PPS types 32/33/34) go to `config`; everything else goes to `data`,
// and a keyframe payload NAL latches isIdr. Buffers shorter than 5 bytes (no
// room for the start code plus a header byte) are ignored.
inline void AppendEncodedNal(PendingEncodedFrame& frame, const uint8_t* data, size_t size,
                             bool isKeyframe, bool h264)
{
    if (data == nullptr || size < 5)
    {
        return;
    }
    const uint8_t nalType = h264 ? (data[4] & 0x1Fu) : ((data[4] >> 1) & 0x3Fu);
    const bool isConfig = h264 ? (nalType == 7 || nalType == 8)
                               : (nalType == 32 || nalType == 33 || nalType == 34);
    auto& buffer = isConfig ? frame.config : frame.data;
    buffer.insert(buffer.end(), data, data + size);
    if (isKeyframe && !isConfig)
    {
        frame.isIdr = true;
    }
}

} // namespace oxrsys::alvr
