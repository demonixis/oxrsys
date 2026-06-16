# SPDX-License-Identifier: MPL-2.0

include_guard(DIRECTORY)

set(FFMPEG_ROOT "" CACHE PATH "Root directory for FFmpeg headers and import libraries")
set(OXRSYS_FFMPEG_LINK_MODE "AUTO" CACHE STRING "FFmpeg link mode: AUTO, STATIC, or DYNAMIC")
set_property(CACHE OXRSYS_FFMPEG_LINK_MODE PROPERTY STRINGS AUTO STATIC DYNAMIC)
string(TOUPPER "${OXRSYS_FFMPEG_LINK_MODE}" OXRSYS_FFMPEG_LINK_MODE_NORMALIZED)
if(NOT OXRSYS_FFMPEG_LINK_MODE_NORMALIZED MATCHES "^(AUTO|STATIC|DYNAMIC)$")
    message(FATAL_ERROR "OXRSYS_FFMPEG_LINK_MODE must be AUTO, STATIC, or DYNAMIC")
endif()

set(_oxrsys_ffmpeg_static_link OFF)
if(OXRSYS_FFMPEG_LINK_MODE_NORMALIZED STREQUAL "STATIC")
    set(_oxrsys_ffmpeg_static_link ON)
elseif(OXRSYS_FFMPEG_LINK_MODE_NORMALIZED STREQUAL "AUTO" AND
       VCPKG_TARGET_TRIPLET MATCHES "-static($|-)")
    set(_oxrsys_ffmpeg_static_link ON)
endif()

function(_oxrsys_select_ffmpeg_binary_dir)
    set(_debug_binary_dirs)
    set(_release_binary_dirs)
    foreach(_oxrsys_ffmpeg_library_dir IN LISTS ARGN)
        if(NOT _oxrsys_ffmpeg_library_dir)
            continue()
        endif()
        get_filename_component(_oxrsys_ffmpeg_library_parent "${_oxrsys_ffmpeg_library_dir}" DIRECTORY)
        if(_oxrsys_ffmpeg_library_dir MATCHES "[/\\\\]debug[/\\\\]lib$")
            if(EXISTS "${_oxrsys_ffmpeg_library_parent}/bin")
                list(APPEND _debug_binary_dirs "${_oxrsys_ffmpeg_library_parent}/bin")
            endif()
        elseif(EXISTS "${_oxrsys_ffmpeg_library_parent}/bin")
            list(APPEND _release_binary_dirs "${_oxrsys_ffmpeg_library_parent}/bin")
        endif()
    endforeach()

    if(CMAKE_BUILD_TYPE MATCHES "^[Dd]ebug" AND _debug_binary_dirs)
        list(GET _debug_binary_dirs 0 _selected_binary_dir)
    elseif(_release_binary_dirs)
        list(GET _release_binary_dirs 0 _selected_binary_dir)
    elseif(_debug_binary_dirs)
        list(GET _debug_binary_dirs 0 _selected_binary_dir)
    endif()

    if(_selected_binary_dir)
        set(OXRSYS_FFMPEG_BINARY_DIR "${_selected_binary_dir}" CACHE PATH
            "Directory containing the FFmpeg runtime DLLs" FORCE)
    endif()
endfunction()

set(OXRSYS_FFMPEG_FOUND FALSE)

if(NOT FFMPEG_ROOT AND NOT "$ENV{FFMPEG_ROOT}" AND NOT "$ENV{FFMPEG_DIR}")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        set(_oxrsys_previous_pkg_config_argn "${PKG_CONFIG_ARGN}")
        if(_oxrsys_ffmpeg_static_link)
            set(PKG_CONFIG_ARGN "--static")
        endif()
        pkg_check_modules(OXRSYS_FFMPEG_PKG QUIET IMPORTED_TARGET GLOBAL
            libavcodec
            libavutil
            libswscale
        )
        set(PKG_CONFIG_ARGN "${_oxrsys_previous_pkg_config_argn}")

        if(OXRSYS_FFMPEG_PKG_FOUND)
            set(OXRSYS_FFMPEG_FOUND TRUE)
            set(OXRSYS_FFMPEG_INCLUDE_DIRS ${OXRSYS_FFMPEG_PKG_INCLUDE_DIRS})
            set(OXRSYS_FFMPEG_LIBRARIES PkgConfig::OXRSYS_FFMPEG_PKG)
            set(OXRSYS_FFMPEG_LIBRARY_DIRS ${OXRSYS_FFMPEG_PKG_LIBRARY_DIRS})
            if(_oxrsys_ffmpeg_static_link)
                set(OXRSYS_FFMPEG_BINARY_DIR "" CACHE PATH
                    "Directory containing the FFmpeg runtime DLLs" FORCE)
                message(STATUS "Using static FFmpeg through pkg-config")
            else()
                _oxrsys_select_ffmpeg_binary_dir(${OXRSYS_FFMPEG_LIBRARY_DIRS})
                message(STATUS "Using FFmpeg through pkg-config")
            endif()
        endif()
    endif()
endif()

if(_oxrsys_ffmpeg_static_link AND NOT OXRSYS_FFMPEG_FOUND)
    message(FATAL_ERROR
        "Static FFmpeg linking requires pkg-config metadata from vcpkg. "
        "Use scripts/windows_configure.ps1 without -FFmpegRoot, or pass "
        "-DynamicFFmpeg to use the dynamic vcpkg FFmpeg triplet.")
endif()

set(_oxrsys_ffmpeg_root_hints
    "${FFMPEG_ROOT}"
    "$ENV{FFMPEG_ROOT}"
    "$ENV{FFMPEG_DIR}"
    "$ENV{ProgramFiles}/FFmpeg"
    "C:/ffmpeg"
    "C:/dev/ffmpeg"
    "D:/ffmpeg"
    "D:/dev/ffmpeg"
)
list(APPEND _oxrsys_ffmpeg_root_hints ${CMAKE_PREFIX_PATH})
if(VCPKG_TARGET_TRIPLET)
    list(APPEND _oxrsys_ffmpeg_root_hints
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}"
    )
    if(VCPKG_INSTALLED_DIR)
        list(APPEND _oxrsys_ffmpeg_root_hints
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        )
    endif()
    if(_VCPKG_INSTALLED_DIR)
        list(APPEND _oxrsys_ffmpeg_root_hints
            "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        )
    endif()
endif()

set(_oxrsys_ffmpeg_include_hints)
set(_oxrsys_ffmpeg_library_hints)
set(_oxrsys_ffmpeg_binary_hints)
foreach(_oxrsys_ffmpeg_root IN LISTS _oxrsys_ffmpeg_root_hints)
    if(_oxrsys_ffmpeg_root)
        list(APPEND _oxrsys_ffmpeg_include_hints
            "${_oxrsys_ffmpeg_root}/include"
            "${_oxrsys_ffmpeg_root}/installed/${CMAKE_VS_PLATFORM_NAME}/include"
        )
        list(APPEND _oxrsys_ffmpeg_library_hints
            "${_oxrsys_ffmpeg_root}/debug/lib"
            "${_oxrsys_ffmpeg_root}/lib"
            "${_oxrsys_ffmpeg_root}/lib/x64"
            "${_oxrsys_ffmpeg_root}/installed/${CMAKE_VS_PLATFORM_NAME}/debug/lib"
            "${_oxrsys_ffmpeg_root}/installed/${CMAKE_VS_PLATFORM_NAME}/lib"
        )
        list(APPEND _oxrsys_ffmpeg_binary_hints
            "${_oxrsys_ffmpeg_root}/debug/bin"
            "${_oxrsys_ffmpeg_root}/bin"
            "${_oxrsys_ffmpeg_root}/installed/${CMAKE_VS_PLATFORM_NAME}/debug/bin"
            "${_oxrsys_ffmpeg_root}/installed/${CMAKE_VS_PLATFORM_NAME}/bin"
        )
    endif()
endforeach()

find_path(OXRSYS_FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    HINTS ${_oxrsys_ffmpeg_include_hints}
)
find_library(OXRSYS_FFMPEG_AVCODEC_LIBRARY
    NAMES avcodec
    HINTS ${_oxrsys_ffmpeg_library_hints}
)
find_library(OXRSYS_FFMPEG_AVUTIL_LIBRARY
    NAMES avutil
    HINTS ${_oxrsys_ffmpeg_library_hints}
)
find_library(OXRSYS_FFMPEG_SWSCALE_LIBRARY
    NAMES swscale
    HINTS ${_oxrsys_ffmpeg_library_hints}
)

if(NOT OXRSYS_FFMPEG_FOUND AND
   OXRSYS_FFMPEG_INCLUDE_DIR AND
   OXRSYS_FFMPEG_AVCODEC_LIBRARY AND
   OXRSYS_FFMPEG_AVUTIL_LIBRARY AND
   OXRSYS_FFMPEG_SWSCALE_LIBRARY)
    set(OXRSYS_FFMPEG_FOUND TRUE)
    set(OXRSYS_FFMPEG_INCLUDE_DIRS "${OXRSYS_FFMPEG_INCLUDE_DIR}")
    set(OXRSYS_FFMPEG_LIBRARIES
        "${OXRSYS_FFMPEG_AVCODEC_LIBRARY}"
        "${OXRSYS_FFMPEG_AVUTIL_LIBRARY}"
        "${OXRSYS_FFMPEG_SWSCALE_LIBRARY}"
    )
    set(OXRSYS_FFMPEG_LIBRARY_DIRS)
    foreach(_oxrsys_ffmpeg_library IN LISTS OXRSYS_FFMPEG_LIBRARIES)
        get_filename_component(_oxrsys_ffmpeg_library_dir "${_oxrsys_ffmpeg_library}" DIRECTORY)
        list(APPEND OXRSYS_FFMPEG_LIBRARY_DIRS "${_oxrsys_ffmpeg_library_dir}")
    endforeach()
    list(REMOVE_DUPLICATES OXRSYS_FFMPEG_LIBRARY_DIRS)
    find_path(OXRSYS_FFMPEG_BINARY_DIR
        NAMES avcodec-62.dll avcodec-61.dll avcodec-60.dll avcodec-59.dll avcodec.dll
        HINTS ${_oxrsys_ffmpeg_binary_hints}
    )
    message(STATUS "Using FFmpeg headers from ${OXRSYS_FFMPEG_INCLUDE_DIR}")
elseif(NOT OXRSYS_FFMPEG_FOUND)
    message(FATAL_ERROR
        "FFmpeg development files were not found. Set -DFFMPEG_ROOT=<path> "
        "or FFMPEG_ROOT to a package containing include/libavcodec/avcodec.h "
        "and import libraries for avcodec, avutil, and swscale under lib/. "
        "On Windows, scripts/windows_configure.ps1 can also enable vcpkg "
        "manifest mode automatically when -FFmpegRoot is not provided.")
endif()
