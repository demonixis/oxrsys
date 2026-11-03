// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <Foveation.h>

namespace oxrsys::streaming_foveation
{

// The foveation preset and layout that the announce and both connect paths
// send and encode with. It is taken together with the encoded size it was
// built for, at Start and when a stream reconfiguration applies, so a
// hot-reloaded foveated_encoding_preset never pairs one preset's parameters
// with another preset's dimensions.
struct Snapshot
{
    oxr::protocol::FoveationPreset preset = oxr::protocol::FoveationPreset::Off;
    oxr::protocol::FoveationLayout layout = {};
};

inline Snapshot TakeSnapshot(bool foveatedEncodingActive,
                             oxr::protocol::FoveationPreset preset,
                             const oxr::protocol::FoveationLayout& layout)
{
    if (!foveatedEncodingActive)
    {
        return {oxr::protocol::FoveationPreset::Off,
                oxr::protocol::CalculateFoveationLayout(layout.targetEyeWidth,
                                                        layout.targetEyeHeight,
                                                        oxr::protocol::FoveationPreset::Off)};
    }
    return {preset, layout};
}

} // namespace oxrsys::streaming_foveation
