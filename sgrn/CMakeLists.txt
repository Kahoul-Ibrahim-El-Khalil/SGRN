# SGRN Meta-Project CMake Configuration
cmake_minimum_required(VERSION 3.25)
project(SGRN_Meta_Project LANGUAGES CXX)

message(STATUS "[SGRN] Configuring meta-project components...")
# Strict warnings for SGRN internal code
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()

# Clang diagnoses implementation details in the MinGW-w64/libstdc++ headers
# bundled with the Win64 cross toolchain.  Keep strict diagnostics enabled for
# SGRN itself, but silence the known third-party header noise on that target.
# These options must follow the strict warning set above because Clang applies
# the last occurrence of an enable/disable pair.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(
        -Wno-c99-extensions
        -Wno-expansion-to-defined
        -Wno-gnu-anonymous-struct
        -Wno-gnu-include-next
        -Wno-implicit-exception-spec-mismatch
        -Wno-int-in-bool-context
        -Wno-mismatched-tags
        -Wno-nested-anon-types
        -Wno-non-virtual-dtor
        -Wno-shadow
        -Wno-switch
        -Wno-unused-but-set-variable
        -Wno-unused-lambda-capture
        -Wno-unused-parameter
        -Wno-unused-value
        -Wno-zero-length-array
    )
endif()

function(sgrn_add_component option_name component_dir)
    if(${option_name})
        message(STATUS "[SGRN] Adding ${component_dir} component")
        add_subdirectory(${component_dir})
    endif()
endfunction()

function(sgrn_add_discovered_component component_dir)
    string(TOUPPER "${component_dir}" _component_upper)
    string(REPLACE "-" "_" _component_upper "${_component_upper}")
    set(_component_option "SGRN_BUILD_${_component_upper}")

    if(NOT DEFINED ${_component_option})
        option(${_component_option} "Build ${component_dir} component" ON)
    endif()

    sgrn_add_component(${_component_option} ${component_dir})
endfunction()

option(SGRN_BUILD_GATEWAY "Build gateway component" ON)

# ------------------------------------------------------------------------------
# Component Orchestration (libraries first, applications last so all library
# targets exist before packaging/install rules run)
# ------------------------------------------------------------------------------
add_subdirectory(lib)

# 3.5 CRUD Library
if(SGRN_BUILD_DATASTORE)
    add_subdirectory(crud)
endif()

# 7. Applications (entry points; libraries first so targets exist)
add_subdirectory(apps)

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(_harvest_targets sgrn_gateway sgrn_replay srgn_dataset)   # <- your real executable targets

    set(_harvest_exes "")
    foreach(_t IN LISTS _harvest_targets)
        if(TARGET ${_t})
            list(APPEND _harvest_exes "$<TARGET_FILE:${_t}>")
        else()
            message(WARNING "[SGRN-PKG/win64] harvest target '${_t}' does not exist")
        endif()
    endforeach()

    # Search dirs: MinGW bin directory plus the toolchain's own bin directory
    get_filename_component(_cc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_harvest_dirs "${_cc_dir}")
    if(DEFINED SGRN_MINGW_BIN_DIR)
        list(APPEND _harvest_dirs "${SGRN_MINGW_BIN_DIR}")
    endif()
    if(CMAKE_FIND_ROOT_PATH)
        foreach(_root IN LISTS CMAKE_FIND_ROOT_PATH)
            list(APPEND _harvest_dirs "${_root}/bin")
        endforeach()
    endif()

    find_program(SGRN_OBJDUMP_EXE
        NAMES ${CMAKE_OBJDUMP} x86_64-w64-mingw32-objdump objdump
        HINTS "${_cc_dir}")

    install(CODE "
        set(SGRN_HARVEST_EXECUTABLES \"${_harvest_exes}\")
        set(SGRN_HARVEST_LIBRARIES   \"\")
        set(SGRN_HARVEST_SEARCH_DIRS \"${_harvest_dirs}\")
        set(SGRN_OBJDUMP             \"${SGRN_OBJDUMP_EXE}\")
        include(\"${CMAKE_SOURCE_DIR}/cmake/scripts/harvest_win64.cmake\")
    ")
endif()
