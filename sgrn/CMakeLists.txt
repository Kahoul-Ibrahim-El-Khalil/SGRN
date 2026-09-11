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
# Component Orchestration (libraries first, applications last so all library
# targets exist before packaging/install rules run)
# ------------------------------------------------------------------------------
add_subdirectory(lib)

# 7. Applications (entry points; libraries first so targets exist)
add_subdirectory(apps)
