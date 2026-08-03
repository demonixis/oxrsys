// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "EncoderTransport.h"
#include "VideoToolboxEncodeEngine.h"

/**
 * In-process IEncoderTransport: a thin forwarding wrapper around
 * VideoToolboxEncodeEngine running in the caller's process. This is the
 * Rosetta H.264 fallback transport; NativeHelperEncoderTransport implements
 * the same seam against an out-of-process native-arm64 encoder helper
 * (this class stays deliberately trivial).
 */
namespace oxrsys::encoder
{

class InProcessEncoderTransport final : public IEncoderTransport
{
public:
    bool Configure(const EncoderConfig& config) override;
    std::shared_ptr<IPendingEncodeFrame> BeginFrame(
        FrameCallbacks callbacks, const EncodedFrameMetrics& seedMetrics) override;
    void ForceKeyframe() override;
    bool SetBitrate(uint32_t bitrateMbps) override;
    void Drain() override;
    void Shutdown() override;

private:
    VideoToolboxEncodeEngine engine_;
};

} // namespace oxrsys::encoder
