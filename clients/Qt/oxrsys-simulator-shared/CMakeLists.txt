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

if(QT_SIMULATOR_FFMPEG_FOUND)
    target_compile_definitions(oxrsys-simulator-shared PUBLIC
        OXRSYS_QT_SIMULATOR_HAS_FFMPEG=1
    )
    target_link_libraries(oxrsys-simulator-shared PRIVATE
        PkgConfig::QT_SIMULATOR_FFMPEG
    )
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
