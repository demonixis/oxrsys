// SPDX-License-Identifier: MPL-2.0

#pragma once

// -----------------------------------------------------------------------------
// The one colour contract every OXRSys VideoToolbox compression session applies,
// shared by the in-process encoder (runtime/src/VideoEncoder.mm, x86_64 or
// arm64) and the out-of-process helper (main.mm, arm64). Both paths encode the
// same 8-bit BGRA compose surface, and the client decodes whatever arrives with
// the colour description written into the SPS VUI. If the two sessions were
// configured separately they could drift (they did: the helper once set none of
// these, so its stream decoded with different colours from the in-process one).
// Keep every colour-affecting session property here, and only here.
//
// SDR BT.709 for every codec and profile, HEVC Main10 included: Main10 is a
// 10-bit bitstream from the same 8-bit SDR source, not an HDR/BT.2020 stream.
//
// Range: video (limited) range on every path. VideoToolbox has no compression
// property for it; a session derives it from its source. Every hardware
// encoder writes video range for the BGRA compose surface. The software HEVC
// encoder (all a Rosetta process gets for HEVC, and so the in-process fallback
// when the helper dies) writes FULL range for a BGRA source once HEVC Main is
// requested explicitly. So a software session is never handed BGRA: its frames
// are first converted to an explicitly video-range 4:2:0 buffer with
// CreateVideoRangeTransferSession(), and the encoder keeps that range. The
// helper only ever runs a hardware session and needs no conversion.
//
// Header-only, system frameworks only, so the helper stays a single translation
// unit with no runtime dependencies.
// -----------------------------------------------------------------------------

#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

namespace oxrsys::encoder_color
{

inline const CFStringRef kPrimaries = kCVImageBufferColorPrimaries_ITU_R_709_2;
inline const CFStringRef kTransferFunction = kCVImageBufferTransferFunction_ITU_R_709_2;
inline const CFStringRef kYCbCrMatrix = kCVImageBufferYCbCrMatrix_ITU_R_709_2;

struct ApplyResult
{
    OSStatus primaries = noErr;
    OSStatus transferFunction = noErr;
    OSStatus yCbCrMatrix = noErr;

    bool ok() const
    {
        return primaries == noErr && transferFunction == noErr && yCbCrMatrix == noErr;
    }
};

// Applies the contract to a freshly created session, before
// VTCompressionSessionPrepareToEncodeFrames. VideoToolbox writes these values
// into the H.264/H.265 VUI and uses the matching matrix for its RGB-to-YCbCr
// conversion. The caller logs a partial failure in its own log.
inline ApplyResult ApplySessionColorProperties(VTCompressionSessionRef session)
{
    ApplyResult result;
    result.primaries =
        VTSessionSetProperty(session, kVTCompressionPropertyKey_ColorPrimaries, kPrimaries);
    result.transferFunction =
        VTSessionSetProperty(session, kVTCompressionPropertyKey_TransferFunction, kTransferFunction);
    result.yCbCrMatrix =
        VTSessionSetProperty(session, kVTCompressionPropertyKey_YCbCrMatrix, kYCbCrMatrix);
    return result;
}

// The source format a software session is fed instead of BGRA. Eight-bit for
// every profile: the compose surface is 8-bit, and HEVC Main10 keeps encoding a
// 10-bit bitstream from it, exactly as with a BGRA source.
inline constexpr OSType kVideoRangeSourcePixelFormat =
    kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;

// A pixel-transfer session that converts the BGRA compose surface into a
// kVideoRangeSourcePixelFormat buffer with the same BT.709 matrix, primaries
// and transfer function the compression session signals, and tags the output
// with them. Returns nullptr on failure; the caller owns the reference.
inline VTPixelTransferSessionRef CreateVideoRangeTransferSession()
{
    VTPixelTransferSessionRef transfer = nullptr;
    if (VTPixelTransferSessionCreate(kCFAllocatorDefault, &transfer) != noErr ||
        transfer == nullptr)
    {
        return nullptr;
    }
    const bool configured =
        VTSessionSetProperty(transfer, kVTPixelTransferPropertyKey_DestinationYCbCrMatrix,
                             kYCbCrMatrix) == noErr &&
        VTSessionSetProperty(transfer, kVTPixelTransferPropertyKey_DestinationColorPrimaries,
                             kPrimaries) == noErr &&
        VTSessionSetProperty(transfer, kVTPixelTransferPropertyKey_DestinationTransferFunction,
                             kTransferFunction) == noErr;
    if (!configured)
    {
        VTPixelTransferSessionInvalidate(transfer);
        CFRelease(transfer);
        return nullptr;
    }
    VTSessionSetProperty(transfer, kVTPixelTransferPropertyKey_RealTime, kCFBooleanTrue);
    return transfer;
}

} // namespace oxrsys::encoder_color
