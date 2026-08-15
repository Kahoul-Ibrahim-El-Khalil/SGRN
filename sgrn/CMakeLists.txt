# SGRN Meta-Project CMake Configuration
cmake_minimum_required(VERSION 3.25)
project(SGRN_Meta_Project LANGUAGES CXX)

message(STATUS "[SGRN] Configuring meta-project components...")
# Strict warnings for SGRN internal code
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
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
# Component Orchestration
# ------------------------------------------------------------------------------

# 1. Core & Utils (Foundational)
if(SGRN_BUILD_UTILS)
    add_subdirectory(core)
    add_subdirectory(codecs)
    add_subdirectory(utils)
endif()

# 2. SDK
sgrn_add_component(SGRN_BUILD_SDK sdk)
sgrn_add_component(SGRN_BUILD_CODECS codecs)

# 3. SCL (Parsing layer)
sgrn_add_component(SGRN_BUILD_GATEWAY scl)

# 3.5 Scripting Engine
sgrn_add_component(SGRN_BUILD_GATEWAY scripting)

# 4. Gateway (Runtime)
sgrn_add_component(SGRN_BUILD_GATEWAY gateway)

# 5. S7Shell (Application entry points)
sgrn_add_component(SGRN_BUILD_S7 s7shell)

# 6. Backend (Infrastructure)
sgrn_add_component(SGRN_BUILD_DATASTORE datastore)
