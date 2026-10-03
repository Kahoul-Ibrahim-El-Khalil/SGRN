cmake_minimum_required(VERSION 3.25)
project(sgrn_crud LANGUAGES CXX)

include(cmake/sgrn_crud_codegen.cmake)

# Collect source files
file(GLOB SGRN_CRUD_SOURCES "src/*.cpp")
file(GLOB SGRN_CRUD_GENERATED "generated/*.gen.hpp")

add_library(sgrn_crud
    ${SGRN_CRUD_SOURCES}
    generated/RegisteredViews.cpp
)

add_library(sgrn::crud ALIAS sgrn_crud)

target_include_directories(sgrn_crud
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(sgrn_crud
    PUBLIC
        sgrn::drogon
        sgrn::trantor
        sgrn::jsoncpp
)

target_compile_features(sgrn_crud PUBLIC cxx_std_20)
