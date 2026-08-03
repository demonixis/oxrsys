// SPDX-License-Identifier: MPL-2.0

#pragma once

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

// Shared fixture helper for the out-of-process encoder tests
// (TestEncoderHelperSmoke.mm's [encoder-helper] GPU smoke and
// TestNativeHelperChaos.mm's [helper-chaos] generation-chaos fixture): both
// need a BGRA, IOSurface-backed CVPixelBuffer to submit through
// NativeHelperEncoderTransport, differing only in size and whether the
// buffer needs to be Metal-compatible (the smoke test blits into it via a
// Metal texture view; the chaos fixture never touches pixel data).

/// Creates a square BGRA CVPixelBuffer backed by an IOSurface (so it can ride
/// the Mach surface rendezvous), `dim` x `dim`. When `metalCompatible` is
/// true, kCVPixelBufferMetalCompatibilityKey is added so the buffer can be
/// wrapped in an MTLTexture for a GPU blit. Returns nullptr on failure.
inline CVPixelBufferRef MakeSurfaceBackedBuffer(size_t dim, bool metalCompatible)
{
    NSMutableDictionary* attrs = [NSMutableDictionary dictionaryWithDictionary:@{
        (NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
    }];
    if (metalCompatible)
    {
        attrs[(NSString*)kCVPixelBufferMetalCompatibilityKey] = @YES;
    }
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferCreate(nullptr, dim, dim, kCVPixelFormatType_32BGRA,
                            (__bridge CFDictionaryRef)attrs, &buffer) != kCVReturnSuccess)
    {
        return nullptr;
    }
    return buffer;
}
