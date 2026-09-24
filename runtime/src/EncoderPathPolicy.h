// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>

#include <oxrsys/protocol/Protocol.h>

/**
 * Where the per-frame VideoToolbox encode runs.
 *
 * The runtime dylib is frequently loaded in-process by an x86_64 (Rosetta) host
 * — CrossOver's Wine host — and VideoToolbox does not grant such a process the
 * hardware HEVC encoder: a session created with RequireHardware=YES fails
 * kVTCouldNotFindVideoEncoderErr, and one created with RequireHardware=NO comes
 * back *silently software* (~27-40 ms/frame). Hardware H.264 is granted under
 * Rosetta; hardware HEVC is not. A native-arm64 helper process is granted both.
 *
 * This header is the decision, isolated from VideoToolbox so it can be tested
 * without a GPU: given the negotiated codec, whether *this* process can really
 * obtain a hardware encoder for it, and the user's override, say which path to
 * take and why. The caller supplies `inProcessHardwareAvailable` from an actual
 * VideoToolbox query (VTCopyVideoEncoderList / a RequireHardware=YES probe) —
 * never from the process architecture, so the policy stays correct if Apple
 * changes what Rosetta is granted.
 */
namespace oxrsys::encoder
{

// `encoder_helper` config key. Auto is the default: the runtime decides from the
// measured hardware-encoder availability. The explicit values exist for
// debugging and for forcing a known-good path in the field.
enum class HelperOverride
{
    Auto = 0,
    ForceHelper = 1,    // encoder_helper = true
    ForceInProcess = 2, // encoder_helper = false
};

enum class EncodePathReason
{
    // -> in-process
    InProcessHardwareAvailable,  // this process gets the hardware encoder: no IPC hop
    HelperDisabledByConfig,      // encoder_helper = false
    CodecUnsupportedByHelper,    // helper cannot encode this codec at all (AV1)
    ForcedButCodecUnsupported,   // encoder_helper = true, but the helper cannot do this codec
    // -> helper
    NoInProcessHardwareEncoder,  // software in-process; the helper is the only hardware path
    HelperForcedByConfig,        // encoder_helper = true
};

struct EncodePathDecision
{
    bool useHelper = false;
    EncodePathReason reason = EncodePathReason::InProcessHardwareAvailable;
};

struct EncodePathInputs
{
    oxr::protocol::VideoCodec codec = oxr::protocol::VideoCodec::H265;
    // HEVC Main10. Not a gate: the helper encodes Main10 from the same 8-bit
    // BGRA compose surface the in-process path uses, exactly as the in-process
    // path does. Carried so the decision can be logged and so a future
    // true-10-bit surface contract has somewhere to hook in.
    bool tenBit = false;
    // Result of an actual VideoToolbox query in THIS process for THIS codec.
    bool inProcessHardwareAvailable = false;
    HelperOverride override_ = HelperOverride::Auto;
};

// True when the out-of-process helper implements the codec at all.
bool HelperSupportsCodec(oxr::protocol::VideoCodec codec);

// Pure decision. No VideoToolbox, no I/O, no globals.
EncodePathDecision ChooseEncodePath(const EncodePathInputs& inputs);

// Stable, log-ready sentence for a reason code.
const char* DescribeEncodePathReason(EncodePathReason reason);

// Parses the `encoder_helper` config value ("auto" / "true" / "false" / ...).
// Anything unrecognised is Auto, so a typo degrades to the sane default.
HelperOverride ParseHelperOverride(const std::string& value);

} // namespace oxrsys::encoder
