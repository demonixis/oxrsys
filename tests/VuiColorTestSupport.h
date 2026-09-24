// SPDX-License-Identifier: MPL-2.0

#pragma once

// Reads the colour description an encoder actually wrote into its bitstream.
//
// CoreMedia parses the SPS VUI when it builds a format description from raw
// parameter sets, so the colour extensions read back here come from the
// encoded bytes, not from any session property that was requested.

#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

#include <cstdint>
#include <string>
#include <vector>

namespace oxrsys::test
{

struct VuiColor
{
    bool present = false;
    std::string primaries;
    std::string transfer;
    std::string matrix;
    bool fullRange = false;
};

inline std::string CFToString(CFTypeRef value)
{
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID())
    {
        return {};
    }
    return [(__bridge NSString*)value UTF8String];
}

// `nals` are Annex-B units (4-byte start code) as the encoder emits them.
inline VuiColor ParseVuiColor(const std::vector<std::vector<uint8_t>>& nals, bool hevc)
{
    std::vector<const uint8_t*> pointers;
    std::vector<size_t> sizes;
    for (const auto& nal : nals)
    {
        if (nal.size() < 5)
        {
            continue;
        }
        const uint8_t* body = nal.data() + 4; // strip the Annex-B start code
        const size_t size = nal.size() - 4;
        const unsigned type = hevc ? ((body[0] >> 1) & 0x3f) : (body[0] & 0x1f);
        const bool parameterSet = hevc ? (type == 32 || type == 33 || type == 34)
                                       : (type == 7 || type == 8);
        if (parameterSet)
        {
            pointers.push_back(body);
            sizes.push_back(size);
        }
        if (pointers.size() == (hevc ? 3u : 2u))
        {
            break;
        }
    }
    VuiColor color;
    if (pointers.size() != (hevc ? 3u : 2u))
    {
        return color;
    }
    CMFormatDescriptionRef format = nullptr;
    const OSStatus status =
        hevc ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                   kCFAllocatorDefault, pointers.size(), pointers.data(), sizes.data(), 4,
                   nullptr, &format)
             : CMVideoFormatDescriptionCreateFromH264ParameterSets(
                   kCFAllocatorDefault, pointers.size(), pointers.data(), sizes.data(), 4,
                   &format);
    if (status != noErr || format == nullptr)
    {
        return color;
    }
    color.present = true;
    color.primaries = CFToString(
        CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_ColorPrimaries));
    color.transfer = CFToString(
        CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_TransferFunction));
    color.matrix = CFToString(
        CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_YCbCrMatrix));
    CFTypeRef fullRange =
        CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FullRangeVideo);
    color.fullRange = fullRange != nullptr && CFGetTypeID(fullRange) == CFBooleanGetTypeID() &&
                      CFBooleanGetValue((CFBooleanRef)fullRange);
    CFRelease(format);
    return color;
}

} // namespace oxrsys::test
