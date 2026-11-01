// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

namespace oxrsys::encode_budget
{

// Picks the foveation level from the measured encode time: when frames queue up behind the encoder,
// it steps to a stronger level. A saturated encoder's average climbs far past the frame period and
// stays there, while a busy app only nudges it over for a moment.
class Controller
{
public:
    static constexpr uint32_t OverloadedWindows = 3;
    static constexpr double OverloadedFraction = 1.15;

    explicit Controller(uint32_t maxLevel) : maxLevel_(maxLevel) {}

    void Reset()
    {
        level_ = 0;
        overloadedWindows_ = 0;
    }

    uint32_t Level() const { return level_; }

    // One measurement window of the current level; returns the level to stream next.
    uint32_t Update(double averageEncodeMs, double framePeriodMs)
    {
        if (averageEncodeMs <= framePeriodMs * OverloadedFraction)
        {
            overloadedWindows_ = 0;
            return level_;
        }
        if (++overloadedWindows_ >= OverloadedWindows && level_ < maxLevel_)
        {
            ++level_;
            overloadedWindows_ = 0;
        }
        return level_;
    }

private:
    uint32_t maxLevel_ = 0;
    uint32_t level_ = 0;
    uint32_t overloadedWindows_ = 0;
};

} // namespace oxrsys::encode_budget
