# deps/CMakeLists.txt — Phase 1: Third-Party Dependency Build
# ─────────────────────────────────────────────────────────────────────────────
# Invoked via preset:   cmake --preset linux-deps-static
# Followed by:          cmake --build .build/linux-deps-static -j$(nproc) --target install
#
# Dependencies are fetched via CMake FetchContent (see extern/cmake/FetchDeps.cmake).
# Source/download cache:  ${CMAKE_SOURCE_DIR}/.build/_deps  (shared across presets)
# Build artifacts:        ${CMAKE_BINARY_DIR}  (preset-specific, isolated)
#
# This project builds ALL third-party libraries and then
# installs them into the SGRN local sysroot via extern/cmake/staging.cmake:
#
#   Output layout:
#     .prefix/include/              ← SGRN_SHARED_PREFIX/include  (all headers)
#     .prefix/<platform>/lib/       ← SGRN_LOCAL_PREFIX/lib       (compiled libs)
#     .prefix/<platform>/lib/cmake/extern/extern-config.cmake
#
# The generated extern-config.cmake is later consumed by the main build
# (cmake --preset linux-static-release) via find_package(extern CONFIG REQUIRED).
#
# Key design decisions:
#   - CMAKE_INSTALL_INCLUDEDIR is OVERRIDDEN globally to SGRN_SHARED_PREFIX/include
#     so no subproject can install headers to a platform-specific location.
#   - All add_subdirectory() calls use EXCLUDE_FROM_ALL so that third-party
#     install() rules are suppressed. Only staging.cmake controls what goes where.
#
# See BUILD_SYSTEM.md §"deps/ — Third-party dependency phase" for details.
# ─────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.25)
project(sgrn_deps LANGUAGES CXX C)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/hardening.cmake)

get_filename_component(SGRN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
include(${SGRN_ROOT}/cmake/global.cmake)
option(SGRN_BUILD_DROGON "Build Drogon alongside Trantor" ON)
option(SGRN_BUILD_EXTERN_CORE "Build core dependencies (Snap7, Open62541)" ON)

# Default options (can be overridden by presets)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(FMT_INSTALL "Install fmt" OFF)
option(FMT_TEST "Build fmt tests" OFF)
option(FMT_DOC "Build fmt docs" OFF)
option(JSONCPP_WITH_TESTS "Build jsoncpp tests" OFF)
option(JSONCPP_WITH_POST_BUILD_UNITTEST "Run jsoncpp post-build tests" OFF)
option(JSONCPP_WITH_PKGCONFIG_SUPPORT "Generate jsoncpp pkg-config" OFF)
option(JSONCPP_WITH_CMAKE_PACKAGE "Generate jsoncpp cmake package" OFF)
option(JSONCPP_WITH_EXAMPLE "Build jsoncpp examples" OFF)
option(UA_ENABLE_AMALGAMATION "Enable open62541 amalgamation" OFF)
option(UA_BUILD_UNIT_TESTS "Build open62541 unit tests" OFF)
option(UA_BUILD_EXAMPLES "Build open62541 examples" OFF)
option(UA_BUILD_TOOLS "Build open62541 tools" OFF)
option(AS_BUILD_SHARED "Build AngelScript as shared library" OFF)
option(AS_BUILD_SAMPLES "Build AngelScript samples" OFF)
option(AS_BUILD_TESTS "Build AngelScript tests" OFF)

if(NOT DEFINED SGRN_BUILD_EXTERN_BACKEND)
    if(SGRN_CROSS_WIN64 OR SGRN_CROSS_ARM64)
        set(SGRN_BUILD_EXTERN_BACKEND OFF CACHE BOOL "Build backend dependencies (Drogon, Trantor, etc.)")
    else()
        set(SGRN_BUILD_EXTERN_BACKEND ON CACHE BOOL "Build backend dependencies (Drogon, Trantor, etc.)")
    endif()
endif()

if(_SGRN_EXTERNAL_LIB_DIR)
    list(APPEND CMAKE_PREFIX_PATH "${SGRN_LOCAL_PREFIX}" "${SGRN_CONDA_PREFIX}" "${SGRN_CONDA_PREFIX}/Library")
    set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

# Force all GNUInstallDirs usage to install headers to the shared prefix
file(RELATIVE_PATH _rel_inc "${CMAKE_INSTALL_PREFIX}" "${SGRN_SHARED_PREFIX}/include")
set(CMAKE_INSTALL_INCLUDEDIR "${_rel_inc}" CACHE PATH "Shared header installation directory" FORCE)

include_guard(GLOBAL)

# Dependencies are sourced via CPM.cmake (see extern/cmake/FetchDeps.cmake).
# sgrn_fetch_source(<dep>) in each dep cmake file triggers the download and
# sets <dep>_SOURCE_DIR. Downloads are cached in CPM_SOURCE_CACHE.
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    # Fallback: if not set by preset, default to a shared location relative to
    # the source tree. This keeps source downloads out of the source tree itself.
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.build/_deps"
        CACHE PATH "Shared FetchContent source/download cache" FORCE)
endif()
message(STATUS "[SGRN/extern] FetchContent source cache: ${FETCHCONTENT_BASE_DIR}")
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/FetchDeps.cmake)

# ── CORE DEPENDENCIES ─────────────────────────────────────────────────────────
if(SGRN_BUILD_EXTERN_CORE)

# Disable third-party install rules across all vendored subprojects by using
# EXCLUDE_FROM_ALL in add_subdirectory calls and disabling their internal
# install toggles. This prevents them from generating unwanted install scripts
# while allowing us to stage them manually at the end of this file.

# Ensure all static libraries in extern/ are built with -fPIC so they can be
# linked into our shared component libraries (like libsgrn_utils.so).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 1. fmt — resolved centrally by cmake/fmt.cmake (single source of truth).
include(${SGRN_ROOT}/cmake/deps/fmt.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/jsoncpp.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/snap7.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/open62541.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/sqlite_modern_cpp.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/angelscript.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/ixwebsocket.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/libmodbus.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/opener.cmake)

endif() # SGRN_BUILD_EXTERN_CORE

# ── Backend Dependencies ──────────────────────────────────────────────────────
if(SGRN_BUILD_EXTERN_BACKEND)
    include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/drogon.cmake)
endif() # SGRN_BUILD_EXTERN_BACKEND

# ── Header-only dependencies ──────────────────────────────────────────────────
# These have no compiled artifacts, but the staged sysroot must still ship their
# headers. The main build in installed-deps mode imports them from the prefix via
# extern-config.cmake — it does NOT re-fetch them from CPM. Fetch the sources here
# so staging.cmake (§2m, guarded by EXISTS checks) can copy their headers into the
# shared prefix. Mirrors the packages wired in cmake/deps/header_only.cmake.
sgrn_fetch_source(rapidjson)
sgrn_fetch_source(cpp_httplib)
sgrn_fetch_source(xml_h)
sgrn_fetch_source(sqlite_modern_cpp)
sgrn_fetch_source(unordered_dense)

# End of third-party section: our own staging follows.

# ── Staging: prefix/<platform> is libraries only ─────────────────────────────
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/staging.cmake)

# Force build of targets that were added with EXCLUDE_FROM_ALL but still
# need to be compiled for the staging install step.
add_custom_target(sgrn_force_build_all ALL)
if(TARGET open62541 AND NOT TARGET extern::open62541_imported)
    add_dependencies(sgrn_force_build_all open62541)
endif()
# cmake/fmt.cmake (Phase A) requested a source-built fmt for win64; ensure it is
# compiled during `--target install` (it is otherwise EXCLUDE_FROM_ALL).
if(SGRN_FMT_FORCE_BUILD AND TARGET fmt)
    add_dependencies(sgrn_force_build_all fmt)
endif()
if(TARGET jsoncpp_static)
    add_dependencies(sgrn_force_build_all jsoncpp_static)
elseif(TARGET jsoncpp_lib)
    add_dependencies(sgrn_force_build_all jsoncpp_lib)
endif()

# ixwebsocket is added with EXCLUDE_FROM_ALL in cmake/deps/ixwebsocket.cmake;
# force it into the ALL build so the staging install step can copy the library.
if(TARGET ixwebsocket)
    add_dependencies(sgrn_force_build_all ixwebsocket)
endif()

if(TARGET drogon)
    add_dependencies(sgrn_force_build_all drogon)
endif()
if(TARGET trantor)
    add_dependencies(sgrn_force_build_all trantor)
endif()
