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
