# SPDX-License-Identifier: MPL-2.0

add_library(oxrsys-simulator-shared STATIC
    src/SimulatorTracking.cpp
    src/SimulatorWidget.cpp
    src/VideoFrameAssembler.cpp
)

target_include_directories(oxrsys-simulator-shared PUBLIC
    src
    ${OXRSYS_PROTOCOL_INCLUDE_DIR}
)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(QT_SIMULATOR_FFMPEG QUIET IMPORTED_TARGET
        libavcodec
        libavutil
        libswscale
    )
endif()

if(NOT QT_SIMULATOR_FFMPEG_FOUND AND WIN32)
    set(FFMPEG_ROOT "" CACHE PATH "Root directory for FFmpeg headers and import libraries on Windows")
    find_path(QT_SIMULATOR_FFMPEG_INCLUDE_DIR
        NAMES libavcodec/avcodec.h
        HINTS
            "${FFMPEG_ROOT}/include"
            "$ENV{FFMPEG_ROOT}/include"
    )
    find_library(QT_SIMULATOR_FFMPEG_AVCODEC_LIBRARY
        NAMES avcodec
        HINTS
            "${FFMPEG_ROOT}/lib"
            "$ENV{FFMPEG_ROOT}/lib"
    )
    find_library(QT_SIMULATOR_FFMPEG_AVUTIL_LIBRARY
        NAMES avutil
        HINTS
            "${FFMPEG_ROOT}/lib"
            "$ENV{FFMPEG_ROOT}/lib"
    )
    find_library(QT_SIMULATOR_FFMPEG_SWSCALE_LIBRARY
        NAMES swscale
        HINTS
            "${FFMPEG_ROOT}/lib"
            "$ENV{FFMPEG_ROOT}/lib"
    )
    if(QT_SIMULATOR_FFMPEG_INCLUDE_DIR AND QT_SIMULATOR_FFMPEG_AVCODEC_LIBRARY AND
       QT_SIMULATOR_FFMPEG_AVUTIL_LIBRARY AND QT_SIMULATOR_FFMPEG_SWSCALE_LIBRARY)
        set(QT_SIMULATOR_FFMPEG_FOUND TRUE)
    endif()
endif()

if(QT_SIMULATOR_FFMPEG_FOUND)
    target_compile_definitions(oxrsys-simulator-shared PUBLIC
        OXRSYS_QT_SIMULATOR_HAS_FFMPEG=1
    )
    target_link_libraries(oxrsys-simulator-shared PRIVATE
        $<$<TARGET_EXISTS:PkgConfig::QT_SIMULATOR_FFMPEG>:PkgConfig::QT_SIMULATOR_FFMPEG>
    )
    if(WIN32 AND QT_SIMULATOR_FFMPEG_INCLUDE_DIR)
        target_include_directories(oxrsys-simulator-shared PRIVATE
            ${QT_SIMULATOR_FFMPEG_INCLUDE_DIR}
        )
        target_link_libraries(oxrsys-simulator-shared PRIVATE
            ${QT_SIMULATOR_FFMPEG_AVCODEC_LIBRARY}
            ${QT_SIMULATOR_FFMPEG_AVUTIL_LIBRARY}
            ${QT_SIMULATOR_FFMPEG_SWSCALE_LIBRARY}
        )
    endif()
endif()

target_link_libraries(oxrsys-simulator-shared PUBLIC
    Qt6::Widgets
    Qt6::Network
)

add_executable(oxrsys-simulator-shared-tests
    tests/SimulatorSharedTests.cpp
)

target_link_libraries(oxrsys-simulator-shared-tests PRIVATE
    oxrsys-simulator-shared
)

add_test(NAME oxrsys-simulator-shared-tests COMMAND oxrsys-simulator-shared-tests)
