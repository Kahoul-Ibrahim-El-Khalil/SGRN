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
    src/AsApiEmitter.cpp
)


sgrn_add_component_library(
    TARGET      sgrn_scl
    ALIAS       sgrn::scl
    TYPE        SHARED
    SOURCES     ${SGRN_SCL_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_LIBS sgrn::core sgrn::utils fmt::fmt sgrn::s7codec sgrn::open62541
    ENABLE_ORIGIN_RPATH
)

target_link_libraries(sgrn_scl PRIVATE
    extern::rapidjson
    extern::xml_h
)



sgrn_target(sgrn_scl)

# =============================================================================
# Tests
# =============================================================================

if(SGRN_BUILD_TESTS)
    sgrn_add_component_executable(
        TARGET schema_binary_codec_test
        SOURCES tests/schema_binary_codec_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_scl
    )
    target_include_directories(schema_binary_codec_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME schema_binary_codec_test COMMAND schema_binary_codec_test)
    sgrn_target(schema_binary_codec_test)
endif()

# =============================================================================
# sclc executable and sclc packaging have moved to sgrn/apps/
# (sclc/ dir + shared packaging block).
# =============================================================================
