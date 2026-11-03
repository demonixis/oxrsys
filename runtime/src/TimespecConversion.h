// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>

#include <cstdint>
#include <ctime>
#include <limits>

// Pure XR_KHR_convert_timespec_time mapping, factored out of EntryPoint so the
// validation is unit-testable without a session. XrTime is a pure offset from a
// CLOCK_MONOTONIC base: XrTime == monoNs - baseNs.
//
// A valid XrTime is strictly positive. Converting zero, a negative time, or a
// time whose CLOCK_MONOTONIC value does not fit returns XR_ERROR_TIME_INVALID, and
// so does a timespec that maps to such an XrTime. A tv_nsec outside [0, 1e9) is a
// malformed timespec and returns XR_ERROR_VALIDATION_FAILURE.
namespace oxrsys
{

constexpr int64_t kNsPerSecond = 1'000'000'000LL;

inline XrResult MonotonicTimespecToXrTime(const struct timespec& ts, int64_t baseNs, XrTime& time)
{
    if (ts.tv_nsec < 0 || ts.tv_nsec >= kNsPerSecond)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (ts.tv_sec < 0 ||
        static_cast<int64_t>(ts.tv_sec) > (std::numeric_limits<int64_t>::max() - ts.tv_nsec) / kNsPerSecond)
    {
        return XR_ERROR_TIME_INVALID;
    }
    const int64_t monoNs = static_cast<int64_t>(ts.tv_sec) * kNsPerSecond + ts.tv_nsec;
    if (monoNs <= baseNs)
    {
        return XR_ERROR_TIME_INVALID;
    }
    time = static_cast<XrTime>(monoNs - baseNs);
    return XR_SUCCESS;
}

inline XrResult XrTimeToMonotonicTimespec(XrTime time, int64_t baseNs, struct timespec& ts)
{
    if (time <= 0 || static_cast<int64_t>(time) > std::numeric_limits<int64_t>::max() - baseNs)
    {
        return XR_ERROR_TIME_INVALID;
    }
    const int64_t monoNs = static_cast<int64_t>(time) + baseNs;
    ts.tv_sec = static_cast<time_t>(monoNs / kNsPerSecond);
    ts.tv_nsec = static_cast<long>(monoNs % kNsPerSecond);
    return XR_SUCCESS;
}

} // namespace oxrsys
