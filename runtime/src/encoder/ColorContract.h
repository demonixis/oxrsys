// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <CoreVideo/CoreVideo.h>

namespace oxrsys::encoder
{

/**
 * Tag a pixel buffer with the stream's fixed BT.709 SDR video-range color
 * contract. Both encode paths must agree: the parent tags its compose-slot
 * buffers, and the helper re-tags each surface it wraps (attachments do not
 * travel with an IOSurface across the process boundary). The matching session
 * properties live in VideoToolboxEncodeEngine::CreateSession.
 */
inline void ApplyBt709ColorAttachments(CVPixelBufferRef pixelBuffer)
{
    CVBufferSetAttachment(pixelBuffer, kCVImageBufferColorPrimariesKey,
                          kCVImageBufferColorPrimaries_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixelBuffer, kCVImageBufferTransferFunctionKey,
                          kCVImageBufferTransferFunction_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixelBuffer, kCVImageBufferYCbCrMatrixKey,
                          kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
}

} // namespace oxrsys::encoder
