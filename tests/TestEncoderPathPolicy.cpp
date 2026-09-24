// SPDX-License-Identifier: MPL-2.0
//
// The encode-path selection policy is the risky new logic in the encoder-helper
// work: it decides, per session, whether the VideoToolbox encode runs in this
// process or in the native-arm64 helper. It is written as a pure function of
// (negotiated codec, profile, measured hardware availability, config override)
// precisely so it can be pinned down here without a GPU, a helper binary or a
// Rosetta host.

#include <catch2/catch_test_macros.hpp>

#include "EncoderPathPolicy.h"

using oxrsys::encoder::ChooseEncodePath;
using oxrsys::encoder::DescribeEncodePathReason;
using oxrsys::encoder::EncodePathDecision;
using oxrsys::encoder::EncodePathInputs;
using oxrsys::encoder::EncodePathReason;
using oxrsys::encoder::HelperOverride;
using oxrsys::encoder::HelperSupportsCodec;
using oxrsys::encoder::ParseHelperOverride;
using oxr::protocol::VideoCodec;

namespace
{

EncodePathDecision Decide(VideoCodec codec, bool hardwareInProcess,
                          HelperOverride override_ = HelperOverride::Auto, bool tenBit = false)
{
    EncodePathInputs inputs;
    inputs.codec = codec;
    inputs.tenBit = tenBit;
    inputs.inProcessHardwareAvailable = hardwareInProcess;
    inputs.override_ = override_;
    return ChooseEncodePath(inputs);
}

} // namespace

TEST_CASE("Encode path policy routes to the helper only when this process lacks hardware")
{
    // The case this whole feature exists for: an x86_64/Rosetta host is refused
    // the hardware HEVC encoder, so the native-arm64 helper is the only way to
    // one.
    const EncodePathDecision rosettaHevc = Decide(VideoCodec::H265, false);
    CHECK(rosettaHevc.useHelper);
    CHECK(rosettaHevc.reason == EncodePathReason::NoInProcessHardwareEncoder);

    // A runtime running natively already has the hardware encoder in-process.
    // Routing through the helper would buy a process hop and an IPC round trip
    // for nothing, so it must not happen.
    const EncodePathDecision nativeHevc = Decide(VideoCodec::H265, true);
    CHECK_FALSE(nativeHevc.useHelper);
    CHECK(nativeHevc.reason == EncodePathReason::InProcessHardwareAvailable);
}

TEST_CASE("Encode path policy keeps H.264 in-process wherever hardware H.264 is granted")
{
    // VideoToolbox grants hardware H.264 even under Rosetta, so the measured
    // availability - not the codec - is what decides.
    const EncodePathDecision grantedH264 = Decide(VideoCodec::H264, true);
    CHECK_FALSE(grantedH264.useHelper);
    CHECK(grantedH264.reason == EncodePathReason::InProcessHardwareAvailable);

    // ...and if some future host is refused it, the helper covers H.264 too.
    const EncodePathDecision refusedH264 = Decide(VideoCodec::H264, false);
    CHECK(refusedH264.useHelper);
    CHECK(refusedH264.reason == EncodePathReason::NoInProcessHardwareEncoder);
}

TEST_CASE("Encode path policy treats HEVC Main10 exactly like Main")
{
    // Main10 is a bitstream profile over the same 8-bit BGRA compose surface, so
    // it is not a reason to skip the helper - the old branch that did was
    // needlessly conservative.
    const EncodePathDecision tenBitRosetta = Decide(VideoCodec::H265, false,
                                                    HelperOverride::Auto, /*tenBit=*/true);
    CHECK(tenBitRosetta.useHelper);
    CHECK(tenBitRosetta.reason == EncodePathReason::NoInProcessHardwareEncoder);

    const EncodePathDecision tenBitNative = Decide(VideoCodec::H265, true, HelperOverride::Auto,
                                                   /*tenBit=*/true);
    CHECK_FALSE(tenBitNative.useHelper);
}

TEST_CASE("Encode path policy never routes a codec the helper cannot encode")
{
    // No Apple Silicon part exposes an AV1 encoder to VideoToolbox, and the
    // helper does not implement one. Honouring the negotiated codec matters more
    // than reaching hardware, so AV1 stays in-process whatever the override says.
    CHECK_FALSE(HelperSupportsCodec(VideoCodec::AV1));
    CHECK(HelperSupportsCodec(VideoCodec::H264));
    CHECK(HelperSupportsCodec(VideoCodec::H265));

    const EncodePathDecision autoAv1 = Decide(VideoCodec::AV1, false);
    CHECK_FALSE(autoAv1.useHelper);
    CHECK(autoAv1.reason == EncodePathReason::CodecUnsupportedByHelper);

    const EncodePathDecision forcedAv1 = Decide(VideoCodec::AV1, false, HelperOverride::ForceHelper);
    CHECK_FALSE(forcedAv1.useHelper);
    CHECK(forcedAv1.reason == EncodePathReason::ForcedButCodecUnsupported);
}

TEST_CASE("Encode path policy honours the encoder_helper override in both directions")
{
    // Force on, even where in-process hardware exists (debugging / measuring).
    const EncodePathDecision forcedOn = Decide(VideoCodec::H265, true, HelperOverride::ForceHelper);
    CHECK(forcedOn.useHelper);
    CHECK(forcedOn.reason == EncodePathReason::HelperForcedByConfig);

    // Force off, even where in-process hardware is missing: the user gets the
    // software encoder they asked for rather than a silent override.
    const EncodePathDecision forcedOff =
        Decide(VideoCodec::H265, false, HelperOverride::ForceInProcess);
    CHECK_FALSE(forcedOff.useHelper);
    CHECK(forcedOff.reason == EncodePathReason::HelperDisabledByConfig);

    // Force-off wins for H.264 as well.
    CHECK_FALSE(Decide(VideoCodec::H264, false, HelperOverride::ForceInProcess).useHelper);
}

TEST_CASE("Encode path policy decision is exhaustive over its inputs")
{
    // Every combination must produce a decision with a reason that describes it,
    // and forcing in-process must never yield the helper.
    for (const VideoCodec codec : {VideoCodec::H265, VideoCodec::H264, VideoCodec::AV1})
    {
        for (const bool hardware : {false, true})
        {
            for (const bool tenBit : {false, true})
            {
                for (const HelperOverride override_ : {HelperOverride::Auto,
                                                       HelperOverride::ForceHelper,
                                                       HelperOverride::ForceInProcess})
                {
                    const EncodePathDecision decision = Decide(codec, hardware, override_, tenBit);
                    INFO("codec=" << (int)codec << " hardware=" << hardware
                                  << " tenBit=" << tenBit << " override=" << (int)override_);
                    CHECK(std::string(DescribeEncodePathReason(decision.reason)) != "unknown");
                    if (override_ == HelperOverride::ForceInProcess)
                    {
                        CHECK_FALSE(decision.useHelper);
                    }
                    if (decision.useHelper)
                    {
                        CHECK(HelperSupportsCodec(codec));
                    }
                    // The helper is never chosen automatically when the encode
                    // can already run on hardware in this process.
                    if (override_ == HelperOverride::Auto && hardware)
                    {
                        CHECK_FALSE(decision.useHelper);
                    }
                }
            }
        }
    }
}

TEST_CASE("encoder_helper config values map onto the override")
{
    CHECK(ParseHelperOverride("auto") == HelperOverride::Auto);
    CHECK(ParseHelperOverride("") == HelperOverride::Auto);
    CHECK(ParseHelperOverride("nonsense") == HelperOverride::Auto);
    CHECK(ParseHelperOverride("true") == HelperOverride::ForceHelper);
    CHECK(ParseHelperOverride("TRUE") == HelperOverride::ForceHelper);
    CHECK(ParseHelperOverride("yes") == HelperOverride::ForceHelper);
    CHECK(ParseHelperOverride("1") == HelperOverride::ForceHelper);
    CHECK(ParseHelperOverride("false") == HelperOverride::ForceInProcess);
    CHECK(ParseHelperOverride("Off") == HelperOverride::ForceInProcess);
    CHECK(ParseHelperOverride("0") == HelperOverride::ForceInProcess);
}
