// SPDX-License-Identifier: MPL-2.0

#import "InProcessEncoderTransport.h"

#include <utility>

namespace oxrsys::encoder
{

bool InProcessEncoderTransport::Configure(const EncoderConfig& config)
{
    return engine_.CreateSession(config);
}

std::shared_ptr<IPendingEncodeFrame> InProcessEncoderTransport::BeginFrame(
    FrameCallbacks callbacks, const EncodedFrameMetrics& seedMetrics)
{
    return engine_.BeginFrame(std::move(callbacks), seedMetrics);
}

void InProcessEncoderTransport::ForceKeyframe()
{
    engine_.ForceKeyframe();
}

bool InProcessEncoderTransport::SetBitrate(uint32_t bitrateMbps)
{
    return engine_.SetBitrate(bitrateMbps);
}

void InProcessEncoderTransport::Drain()
{
    engine_.Drain();
}

void InProcessEncoderTransport::Shutdown()
{
    engine_.DestroySession();
}

} // namespace oxrsys::encoder
