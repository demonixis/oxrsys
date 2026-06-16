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

if(WIN32)
    include(${CMAKE_SOURCE_DIR}/cmake/ResolveFFmpeg.cmake)
    if(OXRSYS_FFMPEG_FOUND)
        set(QT_SIMULATOR_FFMPEG_FOUND TRUE)
        set(QT_SIMULATOR_FFMPEG_INCLUDE_DIRS ${OXRSYS_FFMPEG_INCLUDE_DIRS})
        set(QT_SIMULATOR_FFMPEG_LIBRARIES ${OXRSYS_FFMPEG_LIBRARIES})
        set(QT_SIMULATOR_FFMPEG_LIBRARY_DIRS ${OXRSYS_FFMPEG_LIBRARY_DIRS})
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(QT_SIMULATOR_FFMPEG QUIET IMPORTED_TARGET
            libavcodec
            libavutil
            libswscale
        )
    endif()
endif()

if(QT_SIMULATOR_FFMPEG_FOUND)
    target_compile_definitions(oxrsys-simulator-shared PUBLIC
        OXRSYS_QT_SIMULATOR_HAS_FFMPEG=1
    )
    target_link_libraries(oxrsys-simulator-shared PRIVATE
        $<$<TARGET_EXISTS:PkgConfig::QT_SIMULATOR_FFMPEG>:PkgConfig::QT_SIMULATOR_FFMPEG>
    )
    if(WIN32 AND QT_SIMULATOR_FFMPEG_INCLUDE_DIRS)
        target_include_directories(oxrsys-simulator-shared PRIVATE
            ${QT_SIMULATOR_FFMPEG_INCLUDE_DIRS}
        )
        if(QT_SIMULATOR_FFMPEG_LIBRARY_DIRS)
            target_link_directories(oxrsys-simulator-shared PRIVATE
                ${QT_SIMULATOR_FFMPEG_LIBRARY_DIRS}
            )
        endif()
        target_link_libraries(oxrsys-simulator-shared PRIVATE
            ${QT_SIMULATOR_FFMPEG_LIBRARIES}
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
oxrsys_configure_qt_test_environment(
    oxrsys-simulator-shared-tests
    ${OXRSYS_FFMPEG_BINARY_DIR}
)
