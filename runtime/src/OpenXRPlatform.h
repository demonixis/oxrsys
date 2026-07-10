// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(XR_USE_GRAPHICS_API_D3D11)
#include <d3d11.h>
#endif
#if defined(XR_USE_GRAPHICS_API_D3D12)
#include <d3d12.h>
#endif
#endif

#if defined(XR_USE_GRAPHICS_API_VULKAN)
#include <vulkan/vulkan.h>
#if defined(__APPLE__)
#include <vulkan/vulkan_metal.h>
#endif
#endif

#if defined(XR_USE_GRAPHICS_API_OPENGL)
#include <GL/gl.h>
#if defined(XR_USE_PLATFORM_XLIB)
#include <X11/Xlib.h>
#include <GL/glx.h>
#endif
#endif

#if !defined(_WIN32)
// Enable XR_KHR_convert_timespec_time declarations (xrConvertTimespecTimeToTimeKHR
// et al.) in openxr_platform.h. wineopenxr substitutes the Win32 QPC extension the
// app requests (XR_KHR_win32_convert_performance_counter_time) onto this one.
#include <ctime>
#ifndef XR_USE_TIMESPEC
#define XR_USE_TIMESPEC
#endif
#endif

#include <openxr/openxr_platform.h>

#if defined(XR_USE_PLATFORM_XLIB) && defined(None)
#undef None
#endif
