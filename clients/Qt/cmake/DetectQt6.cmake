# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

set(_oxrsys_qt6_prefix_hints)

if(DEFINED Qt6_DIR)
    get_filename_component(_oxrsys_qt6_config_dir "${Qt6_DIR}" ABSOLUTE)
    get_filename_component(_oxrsys_qt6_prefix "${_oxrsys_qt6_config_dir}/../../.." ABSOLUTE)
    list(APPEND _oxrsys_qt6_prefix_hints "${_oxrsys_qt6_prefix}")
endif()

if(DEFINED ENV{Qt6_DIR})
    get_filename_component(_oxrsys_qt6_config_dir "$ENV{Qt6_DIR}" ABSOLUTE)
    get_filename_component(_oxrsys_qt6_prefix "${_oxrsys_qt6_config_dir}/../../.." ABSOLUTE)
    list(APPEND _oxrsys_qt6_prefix_hints "${_oxrsys_qt6_prefix}")
endif()

if(DEFINED ENV{QTDIR})
    list(APPEND _oxrsys_qt6_prefix_hints "$ENV{QTDIR}")
endif()

if(DEFINED ENV{HOME})
    if(APPLE)
        file(GLOB _oxrsys_qt6_home_prefixes "$ENV{HOME}/Qt/[0-9]*/*")
    elseif(UNIX)
        file(GLOB _oxrsys_qt6_home_prefixes "$ENV{HOME}/Qt/[0-9]*/*")
    endif()

    foreach(_oxrsys_qt6_home_prefix IN LISTS _oxrsys_qt6_home_prefixes)
        if(IS_DIRECTORY "${_oxrsys_qt6_home_prefix}/lib/cmake/Qt6")
            list(APPEND _oxrsys_qt6_prefix_hints "${_oxrsys_qt6_home_prefix}")
        endif()
    endforeach()
endif()

if(WIN32)
    set(_oxrsys_qt6_windows_roots)
    if(DEFINED ENV{SystemDrive})
        list(APPEND _oxrsys_qt6_windows_roots "$ENV{SystemDrive}/Qt")
    endif()
    if(DEFINED ENV{USERPROFILE})
        list(APPEND _oxrsys_qt6_windows_roots "$ENV{USERPROFILE}/Qt")
    endif()
    list(APPEND _oxrsys_qt6_windows_roots "C:/Qt")

    foreach(_oxrsys_qt6_windows_root IN LISTS _oxrsys_qt6_windows_roots)
        file(GLOB _oxrsys_qt6_windows_prefixes "${_oxrsys_qt6_windows_root}/[0-9]*/*")
        foreach(_oxrsys_qt6_windows_prefix IN LISTS _oxrsys_qt6_windows_prefixes)
            if(IS_DIRECTORY "${_oxrsys_qt6_windows_prefix}/lib/cmake/Qt6")
                list(APPEND _oxrsys_qt6_prefix_hints "${_oxrsys_qt6_windows_prefix}")
            endif()
        endforeach()
    endforeach()
endif()

if(APPLE)
    list(APPEND _oxrsys_qt6_prefix_hints
        /opt/homebrew/opt/qt
        /usr/local/opt/qt
        /opt/local/libexec/qt6
    )
endif()

foreach(_oxrsys_qt6_prefix IN LISTS _oxrsys_qt6_prefix_hints)
    if(IS_DIRECTORY "${_oxrsys_qt6_prefix}" AND NOT "${_oxrsys_qt6_prefix}" IN_LIST CMAKE_PREFIX_PATH)
        list(APPEND CMAKE_PREFIX_PATH "${_oxrsys_qt6_prefix}")
    endif()
endforeach()

unset(_oxrsys_qt6_config_dir)
unset(_oxrsys_qt6_prefix)
unset(_oxrsys_qt6_home_prefix)
unset(_oxrsys_qt6_home_prefixes)
unset(_oxrsys_qt6_prefix_hints)
unset(_oxrsys_qt6_windows_prefix)
unset(_oxrsys_qt6_windows_prefixes)
unset(_oxrsys_qt6_windows_root)
unset(_oxrsys_qt6_windows_roots)

if(NOT COMMAND oxrsys_configure_qt_test_environment)
    function(_oxrsys_qt_binary_dir output_variable)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _oxrsys_qt_config)
        get_target_property(_oxrsys_qt_core_location Qt6::Core
            "IMPORTED_LOCATION_${_oxrsys_qt_config}")
        if(NOT _oxrsys_qt_core_location)
            get_target_property(_oxrsys_qt_core_location Qt6::Core IMPORTED_LOCATION_DEBUG)
        endif()
        if(NOT _oxrsys_qt_core_location)
            get_target_property(_oxrsys_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
        endif()
        if(NOT _oxrsys_qt_core_location)
            get_target_property(_oxrsys_qt_core_location Qt6::Core IMPORTED_LOCATION)
        endif()

        if(_oxrsys_qt_core_location)
            get_filename_component(_oxrsys_qt_binary_dir "${_oxrsys_qt_core_location}" DIRECTORY)
            set(${output_variable} "${_oxrsys_qt_binary_dir}" PARENT_SCOPE)
        else()
            set(${output_variable} "" PARENT_SCOPE)
        endif()
    endfunction()

    function(oxrsys_deploy_qt_runtime target_name)
        if(NOT WIN32)
            return()
        endif()

        _oxrsys_qt_binary_dir(_oxrsys_qt_binary_dir)
        find_program(OXRSYS_WINDEPLOYQT_EXECUTABLE
            NAMES windeployqt6.exe windeployqt.exe
            HINTS "${_oxrsys_qt_binary_dir}"
            NO_DEFAULT_PATH
        )
        if(NOT OXRSYS_WINDEPLOYQT_EXECUTABLE)
            message(WARNING "windeployqt was not found; ${target_name} will need Qt DLLs on PATH.")
            return()
        endif()

        set(_oxrsys_windeployqt_config_arg --release)
        if(CMAKE_BUILD_TYPE MATCHES "^[Dd]ebug")
            set(_oxrsys_windeployqt_config_arg --debug)
        endif()

        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${OXRSYS_WINDEPLOYQT_EXECUTABLE}"
                ${_oxrsys_windeployqt_config_arg}
                --no-compiler-runtime
                --no-translations
                --dir "$<TARGET_FILE_DIR:${target_name}>"
                "$<TARGET_FILE:${target_name}>"
            COMMENT "Deploying Qt runtime DLLs for ${target_name}"
            VERBATIM
        )
    endfunction()

    function(oxrsys_configure_qt_gui_executable target_name)
        if(NOT WIN32)
            return()
        endif()

        set_target_properties(${target_name} PROPERTIES
            WIN32_EXECUTABLE TRUE
        )
        if(TARGET Qt6::EntryPoint)
            target_link_libraries(${target_name} PRIVATE Qt6::EntryPoint)
        elseif(TARGET Qt6::WinMain)
            target_link_libraries(${target_name} PRIVATE Qt6::WinMain)
        endif()
    endfunction()

    function(oxrsys_configure_qt_test_environment test_name)
        if(NOT WIN32)
            return()
        endif()

        set(_oxrsys_qt_path_modifications)
        _oxrsys_qt_binary_dir(_oxrsys_qt_binary_dir)
        if(_oxrsys_qt_binary_dir)
            list(APPEND _oxrsys_qt_path_modifications
                "PATH=path_list_prepend:${_oxrsys_qt_binary_dir}")
        endif()

        foreach(_oxrsys_qt_extra_path IN LISTS ARGN)
            if(_oxrsys_qt_extra_path)
                list(APPEND _oxrsys_qt_path_modifications
                    "PATH=path_list_prepend:${_oxrsys_qt_extra_path}")
            endif()
        endforeach()

        if(_oxrsys_qt_path_modifications)
            set_tests_properties(${test_name} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                ENVIRONMENT_MODIFICATION "${_oxrsys_qt_path_modifications}"
            )
        endif()
    endfunction()
endif()
