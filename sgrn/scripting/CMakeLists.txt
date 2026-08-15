cmake_minimum_required(VERSION 3.20)
project(sgrn_scripting LANGUAGES CXX)

include(${CMAKE_SOURCE_DIR}/cmake/core.cmake)

add_library(sgrn_scripting STATIC src/ScriptHost.cpp)
add_library(sgrn::scripting ALIAS sgrn_scripting)

target_include_directories(sgrn_scripting
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(sgrn_scripting
    PUBLIC
        sgrn_utils
        sgrn::angelscript
        extern::angelscript_addons
)

set_target_properties(sgrn_scripting PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

sgrn_install_component_library(sgrn_scripting)
