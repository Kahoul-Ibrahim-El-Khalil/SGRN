# cmake/targets.cmake — High-level component build helpers for SGRN.
# ─────────────────────────────────────────────────────────────────────────────
# Included by: cmake/global.cmake (via CMakeLists.txt)
# Used in    : every sgrn/<component>/CMakeLists.txt
#
# Provides three public-facing helper functions that standardize how SGRN
# components declare their build targets:
#
#   sgrn_add_component_library(TARGET … TYPE … SOURCES … PUBLIC_LIBS … …)
#     Creates a STATIC or SHARED library. Applies INCLUDE_DIR with generator
#     expressions so paths are correct for both in-tree builds and installs.
#     Optionally applies a PCH, sets RPATH for shared libs, and links Windows
#     system libraries when cross-compiling for Win64.
#
#   sgrn_add_component_executable(TARGET … SOURCES … PRIVATE_LIBS … …)
#     Creates an executable using the same conventions.
#
#   sgrn_install_component_library(TARGET … EXPORT_NAME … INCLUDE_SOURCE …)
#   sgrn_install_component_executable(TARGET … COMPONENT …)
#     Install rules for dev-mode distribution (only active when
#     SGRN_INSTALL_DEV=ON). Used by staging_sgrn.cmake to populate .prefix/.
# ─────────────────────────────────────────────────────────────────────────────
include_guard(GLOBAL)

include(GNUInstallDirs)

option(SGRN_INSTALL_DEV "Install headers/archives/CMake targets (dev mode)" OFF)

function(sgrn_enable_origin_rpath target)
    if(UNIX AND NOT APPLE)
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN"
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH_USE_LINK_PATH FALSE
        )
    endif()
endfunction()

function(sgrn_link_windows_system_libs target)
    set(options "")
    set(oneValueArgs SCOPE)
    set(multiValueArgs LIBS)
    cmake_parse_arguments(SGRN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGRN_SCOPE)
        set(SGRN_SCOPE PRIVATE)
    endif()

    if((WIN32 OR MINGW OR CMAKE_SYSTEM_NAME STREQUAL "Windows") AND SGRN_LIBS)
        target_link_libraries(${target} ${SGRN_SCOPE} ${SGRN_LIBS})
    endif()
endfunction()

function(sgrn_add_component_library)
    set(options ENABLE_ORIGIN_RPATH)
    set(oneValueArgs TARGET ALIAS TYPE INCLUDE_DIR PCH)
    set(multiValueArgs SOURCES HEADERS PUBLIC_LIBS PRIVATE_LIBS INTERFACE_LIBS WIN_LIBS)
    cmake_parse_arguments(SGRN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGRN_TARGET OR NOT SGRN_TYPE)
        message(FATAL_ERROR "sgrn_add_component_library requires TARGET and TYPE")
    endif()

    if(SGRN_BUILD_STATIC)
        set(SGRN_TYPE STATIC)
    endif()

    add_library(${SGRN_TARGET} ${SGRN_TYPE} ${SGRN_SOURCES} ${SGRN_HEADERS})
    
    if(SGRN_TYPE STREQUAL "SHARED" AND (WIN32 OR MINGW OR CMAKE_SYSTEM_NAME STREQUAL "Windows"))
        set_target_properties(${SGRN_TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    endif()

    if(SGRN_ALIAS)
        add_library(${SGRN_ALIAS} ALIAS ${SGRN_TARGET})
    endif()

    if(SGRN_INCLUDE_DIR)
        target_include_directories(${SGRN_TARGET} PUBLIC
            $<BUILD_INTERFACE:${SGRN_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:include>
        )
    endif()

    if(SGRN_PCH)
        sgrn_use_pch(${SGRN_TARGET} ${SGRN_PCH})
    endif()

    if(SGRN_PUBLIC_LIBS)
        target_link_libraries(${SGRN_TARGET} PUBLIC ${SGRN_PUBLIC_LIBS})
    endif()

    if(SGRN_PRIVATE_LIBS)
        target_link_libraries(${SGRN_TARGET} PRIVATE ${SGRN_PRIVATE_LIBS})
    endif()

    if(SGRN_INTERFACE_LIBS)
        target_link_libraries(${SGRN_TARGET} INTERFACE ${SGRN_INTERFACE_LIBS})
    endif()

    if(SGRN_WIN_LIBS)
        sgrn_link_windows_system_libs(${SGRN_TARGET} SCOPE PUBLIC LIBS ${SGRN_WIN_LIBS})
    endif()

    if(SGRN_ENABLE_ORIGIN_RPATH)
        sgrn_enable_origin_rpath(${SGRN_TARGET})
    endif()
endfunction()

function(sgrn_install_component_library)
    set(options "")
    set(oneValueArgs TARGET EXPORT_NAME INCLUDE_SOURCE INCLUDE_DESTINATION)
    set(multiValueArgs "")
    cmake_parse_arguments(SGRN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGRN_INSTALL_DEV)
        return()
    endif()

    if(NOT SGRN_TARGET OR NOT SGRN_EXPORT_NAME)
        message(FATAL_ERROR "sgrn_install_component_library requires TARGET and EXPORT_NAME")
    endif()

    install(TARGETS ${SGRN_TARGET}
        EXPORT ${SGRN_EXPORT_NAME}-targets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION .
        RUNTIME DESTINATION .
        INCLUDES DESTINATION include
    )

    install(EXPORT ${SGRN_EXPORT_NAME}-targets
        FILE ${SGRN_EXPORT_NAME}Targets.cmake
        NAMESPACE sgrn::
        DESTINATION lib/cmake/${SGRN_EXPORT_NAME}
    )

    if(SGRN_INCLUDE_SOURCE AND SGRN_INCLUDE_DESTINATION)
        install(DIRECTORY ${SGRN_INCLUDE_SOURCE}
            DESTINATION ${SGRN_INCLUDE_DESTINATION}
            FILES_MATCHING PATTERN "*.hpp"
        )
    endif()
endfunction()

function(sgrn_add_component_executable)
    set(options ENABLE_ORIGIN_RPATH)
    set(oneValueArgs TARGET PCH)
    set(multiValueArgs SOURCES PUBLIC_LIBS PRIVATE_LIBS WIN_LIBS)
    cmake_parse_arguments(SGRN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGRN_TARGET OR NOT SGRN_SOURCES)
        message(FATAL_ERROR "sgrn_add_component_executable requires TARGET and SOURCES")
    endif()

    add_executable(${SGRN_TARGET} ${SGRN_SOURCES})

    if(SGRN_PCH)
        sgrn_use_pch(${SGRN_TARGET} ${SGRN_PCH})
    endif()

    if(SGRN_PUBLIC_LIBS)
        target_link_libraries(${SGRN_TARGET} PUBLIC ${SGRN_PUBLIC_LIBS})
    endif()

    if(SGRN_PRIVATE_LIBS)
        target_link_libraries(${SGRN_TARGET} PRIVATE ${SGRN_PRIVATE_LIBS})
    endif()

    if(SGRN_WIN_LIBS)
        sgrn_link_windows_system_libs(${SGRN_TARGET} SCOPE PRIVATE LIBS ${SGRN_WIN_LIBS})
    endif()

    if(SGRN_ENABLE_ORIGIN_RPATH)
        sgrn_enable_origin_rpath(${SGRN_TARGET})
    endif()
endfunction()

function(sgrn_install_component_executable)
    set(options "")
    set(oneValueArgs TARGET COMPONENT)
    set(multiValueArgs "")
    cmake_parse_arguments(SGRN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGRN_INSTALL_DEV)
        return()
    endif()

    if(NOT SGRN_TARGET)
        message(FATAL_ERROR "sgrn_install_component_executable requires TARGET")
    endif()

    set(_component_args)
    if(SGRN_COMPONENT)
        list(APPEND _component_args COMPONENT ${SGRN_COMPONENT})
    endif()

    install(TARGETS ${SGRN_TARGET}
        RUNTIME DESTINATION .
        ${_component_args}
    )
endfunction()
