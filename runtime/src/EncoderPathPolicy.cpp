// SPDX-License-Identifier: MPL-2.0

#include "EncoderPathPolicy.h"

#include <cctype>

namespace oxrsys::encoder
{

bool HelperSupportsCodec(oxr::protocol::VideoCodec codec)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
        case oxr::protocol::VideoCodec::H265:
            return true;
        case oxr::protocol::VideoCodec::AV1:
        default:
            // No Apple Silicon part exposes an AV1 *encoder* to VideoToolbox
            // (hardware AV1 decode exists from M3; encode does not), and the
            // in-process VideoToolbox path does not implement AV1 either.
            return false;
    }
}

EncodePathDecision ChooseEncodePath(const EncodePathInputs& inputs)
{
    const bool helperCanEncode = HelperSupportsCodec(inputs.codec);

    // A codec the helper cannot encode settles it before anything else: routing
    // there would mean silently substituting a codec the client never
    // negotiated, which is worse than a slow encode.
    if (!helperCanEncode)
    {
        return {false, inputs.override_ == HelperOverride::ForceHelper
                           ? EncodePathReason::ForcedButCodecUnsupported
                           : EncodePathReason::CodecUnsupportedByHelper};
    }

    switch (inputs.override_)
    {
        case HelperOverride::ForceInProcess:
            return {false, EncodePathReason::HelperDisabledByConfig};
        case HelperOverride::ForceHelper:
            return {true, EncodePathReason::HelperForcedByConfig};
        case HelperOverride::Auto:
        default:
            break;
    }

    // Auto. The helper exists to reach a hardware encoder this process is
    // refused. When the process already has one, the helper would only add a
    // process hop, an IPC round trip and a second VideoToolbox session for no
    // gain — so a natively running runtime keeps the in-process path.
    if (inputs.inProcessHardwareAvailable)
    {
        return {false, EncodePathReason::InProcessHardwareAvailable};
    }
    return {true, EncodePathReason::NoInProcessHardwareEncoder};
}

const char* DescribeEncodePathReason(EncodePathReason reason)
{
    switch (reason)
    {
        case EncodePathReason::InProcessHardwareAvailable:
            return "this process has a hardware encoder for the negotiated codec";
        case EncodePathReason::HelperDisabledByConfig:
            return "encoder_helper = false";
        case EncodePathReason::CodecUnsupportedByHelper:
            return "the helper does not implement the negotiated codec";
        case EncodePathReason::ForcedButCodecUnsupported:
            return "encoder_helper = true, but the helper does not implement the negotiated codec";
        case EncodePathReason::NoInProcessHardwareEncoder:
            return "this process is refused a hardware encoder for the negotiated codec";
        case EncodePathReason::HelperForcedByConfig:
            return "encoder_helper = true";
    }
    return "unknown";
}

HelperOverride ParseHelperOverride(const std::string& value)
{
    std::string lowered;
    lowered.reserve(value.size());
    for (char character : value)
    {
        lowered.push_back((char)std::tolower((unsigned char)character));
    }

    if (lowered == "true" || lowered == "yes" || lowered == "on" || lowered == "1" ||
        lowered == "always" || lowered == "force")
    {
        return HelperOverride::ForceHelper;
    }
    if (lowered == "false" || lowered == "no" || lowered == "off" || lowered == "0" ||
        lowered == "never")
    {
        return HelperOverride::ForceInProcess;
    }
    return HelperOverride::Auto;
}

} // namespace oxrsys::encoder
