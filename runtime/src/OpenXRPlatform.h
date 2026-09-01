// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>

#if defined(XR_USE_GRAPHICS_API_VULKAN)
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>
#endif

// Enable the XR_KHR_convert_timespec_time declarations
// (xrConvertTimespecTimeToTimeKHR et al.) in openxr_platform.h.
#include <ctime>
#ifndef XR_USE_TIMESPEC
#define XR_USE_TIMESPEC
#endif

#include <openxr/openxr_platform.h>
