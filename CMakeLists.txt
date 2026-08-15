cmake_minimum_required(VERSION 3.25)

# Silence noisy open62541/cmake warnings
if(POLICY CMP0175)
    cmake_policy(SET CMP0175 OLD)
endif()
if(POLICY CMP0148)
    cmake_policy(SET CMP0148 OLD)
endif()
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

# 0. Environment guard — runs BEFORE project(): compiler discovery needs a
#    working toolchain from the SGRN conda environment, so a forgotten
#    `micromamba activate SGRN` must be reported clearly instead of silently
#    failing inside project().
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/env_guard.cmake)

# 1. Modern Project Definition (Loads toolchain if --preset win64 is used)
project(SGRN_Monorepo LANGUAGES CXX C)

# 3.1 Build Workflow Toggles
#
# `SGRN_DEPS_ONLY`: configure only third-party deps (extern/) and stop. Use this
# with `--target install` to populate `.prefix/<platform>`.
# `SGRN_USE_INSTALLED_DEPS`: skip building deps/ and rely on deps already
# installed into `.prefix/<platform>`.
option(SGRN_DEPS_ONLY "Configure only deps/ dependencies" OFF)
option(SGRN_USE_INSTALLED_DEPS "Use dependencies installed in SGRN_LOCAL_PREFIX" OFF)

# 2. Global SGRN Configuration (Source of Truth)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/global.cmake)

# 2.1 CPM.cmake dependency declarations — loaded early so CPMAddPackage/sgrn_fetch_source
# is in scope for all cmake/ files that consume third-party deps.
# This applies in all three modes: deps-only, installed-deps, and normal build.
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.build/_deps"
        CACHE PATH "Shared FetchContent source/download cache")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps/FetchDeps.cmake)

# 3.2 Feature Options (Formerly in config.cmake)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(USE_PCH "Enable precompiled headers" ON)
option(SGRN_BUILD_UTILS "Build sgrn_utils library" ON)
option(SGRN_BUILD_SDK "Build sgrn_sdk library" ON)
option(SGRN_BUILD_S7 "Build sgrn_s7 tools/library" ON)
option(SGRN_BUILD_OPC "Build sgrn_opc tool/library" ON)
option(SGRN_BUILD_GATEWAY "Build gateway component" ON)

set(_BG_DEFAULT ON)
if(SGRN_CROSS_WIN64 OR SGRN_CROSS_ARM64)
    set(_BG_DEFAULT OFF)
endif()
option(SGRN_BUILD_DATASTORE "Build sgrn backend server" ${_BG_DEFAULT})

if(NOT SGRN_BUILD_DATASTORE OR SGRN_NO_DROGON)
    add_compile_definitions(SGRN_NO_DROGON)
endif()

set(SGRN_BUILD_DROGON ${SGRN_BUILD_DATASTORE} CACHE BOOL "Build Drogon alongside Trantor" FORCE)

# Auto-enable installed-deps mode when the repo-local extern package exists.
if(NOT SGRN_DEPS_ONLY AND NOT SGRN_USE_INSTALLED_DEPS)
    if(EXISTS "${SGRN_LOCAL_PREFIX}/lib/cmake/extern/extern-config.cmake")
        set(SGRN_USE_INSTALLED_DEPS ON CACHE BOOL "Use dependencies installed in SGRN_LOCAL_PREFIX" FORCE)
    endif()
endif()

# 4. Dependency Definition Layer
sgrn_alias(sgrn::zlib)
sgrn_alias(sgrn::zstd)
sgrn_alias(sgrn::openssl)
sgrn_alias(sgrn::sqlite)
sgrn_alias(sgrn::sqlite_modern_cpp)
sgrn_alias(sgrn::cxxopts)
sgrn_alias(sgrn::jsoncpp)
sgrn_alias(sgrn::rapidjson)
sgrn_alias(sgrn::httplib)
sgrn_alias(sgrn::xml_h)
sgrn_alias(sgrn::fmt)
sgrn_alias(sgrn::snap7)
sgrn_alias(sgrn::open62541)
sgrn_alias(sgrn::drogon)
sgrn_alias(sgrn::trantor)
sgrn_alias(sgrn::angelscript)
sgrn_alias(sgrn::angelscript_addons)
sgrn_alias(sgrn::ixwebsocket)
sgrn_alias(sgrn::modbus)
sgrn_alias(sgrn::opener)

sgrn_alias(sgrn::postgres)

sgrn_alias(sgrn::redis)
sgrn_alias(sgrn::aws)

# 5. Build Areas (Extern First)
if(SGRN_CROSS_WIN64 OR SGRN_CROSS_ARM64)
    set(SGRN_SKIP_HOST_DEPS ON)
endif()

if(SGRN_DEPS_ONLY)
    add_subdirectory(deps)
    # message(STATUS "[SGRN] Deps-only mode enabled. Build with `--target install` to populate ${SGRN_LOCAL_PREFIX}.")
    return()
endif()

if(NOT SGRN_USE_INSTALLED_DEPS)
    add_subdirectory(deps)
else()
    # ── Installed-deps mode: import all extern targets via the generated package ──
    # If we're relying on already-staged extern deps in `.prefix/<platform>`, import
    # them before wiring the sgrn::* alias targets. Otherwise the alias wiring would
    # run before the real targets exist (and the link interface would be empty).
    if(SGRN_BUILD_STATIC)
        set(_static_suffix "-static")
    else()
        set(_static_suffix "")
    endif()

    if(SGRN_CROSS_ARM64)
        set(_deps_preset "linux-arm64-deps${_static_suffix}")
    elseif(SGRN_CROSS_WIN64)
        set(_deps_preset "win64-deps${_static_suffix}")
    else()
        set(_deps_preset "linux-deps${_static_suffix}")
    endif()

    find_package(extern CONFIG REQUIRED
        PATHS "${SGRN_LOCAL_PREFIX}/lib/cmake/extern"
        NO_DEFAULT_PATH
    )
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps.cmake)

# Avoid global include path injection. All include paths should come from
# imported/linked targets (fmt/jsoncpp/drogon/etc.) so headers and linked
# libraries remain version-consistent.

include(${CMAKE_CURRENT_LIST_DIR}/cmake/pch.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/targets.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/packaging.cmake)

# Stage SGRN's own libraries/headers into the prefix alongside third-party deps.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/staging_sgrn.cmake)

# (Block moved up)


if(SGRN_BUILD_DATASTORE)
    # Wire sgrn::drogon/trantor to source-built or precompiled targets
    sgrn_link_dependency(sgrn::drogon drogon drogon)
    sgrn_link_dependency(sgrn::trantor trantor trantor)
endif()
sgrn_link_dependency(sgrn::snap7 snap7cpp extern::snap7cpp)
sgrn_link_dependency(sgrn::open62541 extern::open62541 extern::open62541)
sgrn_link_dependency(sgrn::angelscript angelscript angelscript)
sgrn_link_dependency(sgrn::angelscript_addons angelscript_addons extern::angelscript_addons)
sgrn_link_dependency(sgrn::ixwebsocket ixwebsocket::ixwebsocket ixwebsocket::ixwebsocket)
sgrn_link_dependency(sgrn::modbus modbus extern::modbus)
sgrn_link_dependency(sgrn::opener opener extern::opener)

# Map fmt and jsoncpp aliases to extern targets ONLY if they weren't already found in the environment.
if(NOT SGRN_FMT_FROM_ENV)
    sgrn_link_dependency(sgrn::fmt fmt extern::fmt)
endif()
if(NOT SGRN_JSONCPP_FROM_ENV)
    sgrn_link_dependency(sgrn::jsoncpp jsoncpp extern::jsoncpp_lib)
endif()
sgrn_link_dependency(sgrn::rapidjson rapidjson extern::rapidjson)
sgrn_link_dependency(sgrn::httplib httplib extern::httplib)
sgrn_link_dependency(sgrn::xml_h extern::xml_h extern::xml_h)
sgrn_link_dependency(sgrn::sqlite_modern_cpp sqlite_modern_cpp extern::sqlite_modern_cpp)

if(SGRN_USE_INSTALLED_DEPS)
    if(SGRN_BUILD_S7 AND (NOT TARGET extern::snap7cpp OR NOT TARGET extern::angelscript_addons))
        message(FATAL_ERROR "SGRN_USE_INSTALLED_DEPS=ON but required deps are missing in ${SGRN_LOCAL_PREFIX}. Run `cmake --workflow --preset ${_deps_preset}`.")
    endif()
    if(SGRN_BUILD_OPC AND NOT TARGET extern::open62541)
        message(FATAL_ERROR "SGRN_USE_INSTALLED_DEPS=ON but open62541 was not found in ${SGRN_LOCAL_PREFIX}. Run `cmake --workflow --preset ${_deps_preset}`.")
    endif()
endif()

# ── Dependency wiring assertion ─────────────────────────────────────────────
# Every alias listed below MUST be populated (non-empty link interface) now
# that cmake/deps*.cmake and the sgrn_link_dependency() calls above have run.
# An empty alias means no provider was wired — historically that surfaced only
# as cryptic link-time undefined references (e.g. the win64 fmt bug). Assert it
# here so it fails at configure time instead.
set(SGRN_REQUIRED_DEPENDENCIES
    fmt jsoncpp
    # NOTE: sgrn::cxxopts is intentionally NOT asserted — it is conditionally
    # wired (provider varies per platform; absent from the win64 target env) and
    # only consumed by the s7-shell/scl tools.
    rapidjson httplib xml_h sqlite_modern_cpp
    snap7 open62541 angelscript angelscript_addons ixwebsocket modbus opener
    zlib zstd openssl sqlite
)
sgrn_assert_dependencies_wired()

if(USE_PCH)
    sgrn_create_pch_targets()
endif()

option(SGRN_BUILD_TESTS "Build SGRN test suite and utilities" ON)

if(SGRN_BUILD_TESTS)
    enable_testing()
endif()

# sgrn monorepo components
add_subdirectory(sgrn)

# Stage SGRN's own libraries, headers, and generate sgrn-config.cmake
# into the prefix alongside third-party deps.
sgrn_stage_to_prefix()

message(STATUS "==========================================")
message(STATUS "SGRN Monorepo Build Configuration Summary")
message(STATUS "==========================================")
message(STATUS "Build type:           ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler:             ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ Standard:         ${CMAKE_CXX_STANDARD}")
message(STATUS "Cross-compiling:      ${SGRN_CROSS_ARM64} (ARM64) / ${SGRN_CROSS_WIN64} (Win64)")
message(STATUS "Shared Libraries:     ${BUILD_SHARED_LIBS}")
message(STATUS "Build utils:          ${SGRN_BUILD_UTILS}")
message(STATUS "Build sdk:            ${SGRN_BUILD_SDK}")
message(STATUS "Build opc:            ${SGRN_BUILD_OPC}")
message(STATUS "Build scl & s7shell:  ${SGRN_BUILD_S7}")
message(STATUS "Build gateway:        ${SGRN_BUILD_GATEWAY}")
message(STATUS "Build datastore:      ${SGRN_BUILD_DATASTORE}")
message(STATUS "==========================================")
