cmake_minimum_required(VERSION 3.25)
project(sgrn_scl VERSION 1.0.0 LANGUAGES CXX)
include(${CMAKE_SOURCE_DIR}/cmake/core.cmake)

set(SGRN_SCL_SOURCES
    src/utils.cpp
    src/schema/PlcSchemaStore.cpp
    src/schema/SclCompiler.cpp
    src/schema/SchemaSerializer.cpp
    src/schema/DbSymbolsParser.cpp
    src/schema/XmlTagTableParser.cpp
    src/schema/XmlTagTableImplementation.cpp
    src/schema/TagTable.cpp
    src/schema/ModbusMap.cpp
)


sgrn_add_component_library(
    TARGET      sgrn_scl
    ALIAS       sgrn::scl
    TYPE        SHARED
    SOURCES     ${SGRN_SCL_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_LIBS sgrn::core sgrn::utils fmt::fmt
    ENABLE_ORIGIN_RPATH
)

target_link_libraries(sgrn_scl PRIVATE
    extern::rapidjson
    extern::xml_h
    sgrn::s7codec
)



sgrn_target(sgrn_scl)

# =============================================================================
# 2. sclc — SCL Compiler CLI
# =============================================================================

sgrn_add_component_executable(
    TARGET      sclc
    SOURCES     apps/sclcompiler.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_scl
        sgrn::core
        sgrn::cxxopts
)

target_include_directories(sclc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

sgrn_target(sclc)

sgrn_install_component_executable(TARGET sclc COMPONENT sclc)

sgrn_package_runtime_dependencies(
    APPS
        sclc
    LIBS
        sgrn_scl
        sgrn_utils
    COMPONENT sclc
)
