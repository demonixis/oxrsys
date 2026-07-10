# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.22)
project(OXRSysAndroid LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OXRSYS_PREFERRED_DISPLAY_REFRESH_RATE_HZ "72" CACHE STRING
    "Preferred headset display refresh rate requested by the Android client")
set(OXRSYS_BUILD "1" CACHE STRING
    "Global OXRSys build number used by the Android client")
set(OXRSYS_ANDROID_STABLE_RELEASE OFF CACHE BOOL
    "Build the Android client release variant with Quest-stable native flags")
set(OXRSYS_ANDROID_BUILD_PROFILE "Custom" CACHE STRING
    "Android client build profile label logged at startup")
if(NOT "${OXRSYS_BUILD}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR "OXRSYS_BUILD must be an integer")
endif()
if(OXRSYS_ANDROID_BUILD_PROFILE STREQUAL "Debug")
    set(OXRSYS_ANDROID_BUILD_PROFILE_LABEL "Debug")
elseif(OXRSYS_ANDROID_BUILD_PROFILE STREQUAL "ReleaseStable")
    set(OXRSYS_ANDROID_BUILD_PROFILE_LABEL "Release stable")
elseif(OXRSYS_ANDROID_BUILD_PROFILE STREQUAL "OptimizedRelease")
    set(OXRSYS_ANDROID_BUILD_PROFILE_LABEL "OptimizedRelease")
else()
    set(OXRSYS_ANDROID_BUILD_PROFILE_LABEL "${OXRSYS_ANDROID_BUILD_PROFILE}")
endif()

# ─── OpenXR SDK (headers + loader) ──────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    OpenXR
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.57
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(OpenXR)

# ─── Android native_app_glue ────────────────────────────────────────────────
# Provided by the NDK — gives us android_app struct and app lifecycle
set(NATIVE_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(native_app_glue STATIC
    ${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c
)
target_include_directories(native_app_glue PUBLIC
    ${NATIVE_APP_GLUE_DIR}
)
target_link_libraries(native_app_glue PUBLIC
    android
    log
)

# ─── Shared protocol definitions ────────────────────────────────────────────
set(OXRSYS_PROTOCOL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../common/protocol/include")

# ─── Native client library ──────────────────────────────────────────────────
add_library(oxrsys_android SHARED
    main.cpp
    XrApp.cpp
    QuestPassthroughAlphaPolicy.cpp
    QuestShellInteraction.cpp
    VideoDecoder.cpp
    NetworkReceiver.cpp
    TrackingSender.cpp
)

target_include_directories(oxrsys_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OXRSYS_PROTOCOL_INCLUDE_DIR}
    ${openxr_SOURCE_DIR}/include
)

target_link_libraries(oxrsys_android PRIVATE
    -Wl,--whole-archive native_app_glue -Wl,--no-whole-archive
    android
    log
    mediandk
    nativewindow
    EGL
    GLESv3
    openxr_loader
)

if(OXRSYS_ANDROID_STABLE_RELEASE)
    target_compile_options(oxrsys_android PRIVATE
        -O0
        -g
        -fno-omit-frame-pointer
        -UNDEBUG
    )
elseif(OXRSYS_ANDROID_BUILD_PROFILE STREQUAL "OptimizedRelease")
    target_compile_options(oxrsys_android PRIVATE
        -O2
        -DNDEBUG
    )
endif()

target_compile_definitions(oxrsys_android PRIVATE
    XR_USE_PLATFORM_ANDROID
    XR_USE_GRAPHICS_API_OPENGL_ES
    OXRSYS_BUILD=${OXRSYS_BUILD}
    OXRSYS_PREFERRED_DISPLAY_REFRESH_RATE_HZ=${OXRSYS_PREFERRED_DISPLAY_REFRESH_RATE_HZ}
    OXRSYS_ANDROID_STABLE_RELEASE=$<IF:$<BOOL:${OXRSYS_ANDROID_STABLE_RELEASE}>,1,0>
    OXRSYS_ANDROID_BUILD_PROFILE_LABEL="${OXRSYS_ANDROID_BUILD_PROFILE_LABEL}"
)
