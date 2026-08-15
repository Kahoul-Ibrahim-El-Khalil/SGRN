# cmake/global.cmake — SGRN Global Configuration (Source of Truth)
# ─────────────────────────────────────────────────────────────────────────────
# Included by: CMakeLists.txt (root), extern/CMakeLists.txt
# Must be included before any other cmake/* file.
#
# Responsibilities:
#   1. Detect the active platform/architecture (linux, win64, linux-arm64).
#   2. Define the two-tier sysroot paths:
#        SGRN_SHARED_PREFIX  = ${SGRN_ROOT}/.prefix
#          └─ include/       All headers (architecture-neutral, one copy)
#        SGRN_LOCAL_PREFIX   = ${SGRN_ROOT}/.prefix/<platform>
#          └─ lib/           Compiled archives/shared libs (platform-specific)
#   3. Locate the active Conda/Micromamba environment (SGRN_CONDA_PREFIX).
#   4. Include cmake/macros.cmake (sgrn_alias, sgrn_link_dependency, …).
#   5. Include cmake/targets.cmake (sgrn_add_component_library, …).
#
# See BUILD_SYSTEM.md for the full sysroot layout and variable reference.
# ─────────────────────────────────────────────────────────────────────────────

include_guard(GLOBAL)

# 1. Platform & Architecture Detection
# ------------------------------------------------------------------------------
# Targets: win64, linux, linux-arm64, etc.
#
# Presets are the source of truth. If `SGRN_PLATFORM` is defined by the preset,
# honor it and derive cross flags from it. Otherwise fall back to detection from
# the active toolchain.

if(DEFINED SGRN_PLATFORM AND NOT SGRN_PLATFORM STREQUAL "")
    if(SGRN_PLATFORM MATCHES "win64")
        set(SGRN_CROSS_WIN64 ON CACHE BOOL "Are we cross-compiling for Windows?" FORCE)
        set(SGRN_CROSS_ARM64 OFF CACHE BOOL "Are we cross-compiling for ARM64?" FORCE)
    elseif(SGRN_PLATFORM MATCHES "linux-arm64")
        set(SGRN_CROSS_WIN64 OFF CACHE BOOL "Are we cross-compiling for Windows?" FORCE)
        set(SGRN_CROSS_ARM64 ON CACHE BOOL "Are we cross-compiling for ARM64?" FORCE)
    else()
        set(SGRN_CROSS_WIN64 OFF CACHE BOOL "Are we cross-compiling for Windows?" FORCE)
        set(SGRN_CROSS_ARM64 OFF CACHE BOOL "Are we cross-compiling for ARM64?" FORCE)
    endif()
else()
    if(CMAKE_SYSTEM_NAME MATCHES "^(Windows|MinGW)$" OR MINGW OR WIN32 OR SGRN_CROSS_WIN64)
        set(SGRN_PLATFORM_NAME "windows")
        set(SGRN_ARCH "win64")
        set(SGRN_CROSS_WIN64 ON CACHE BOOL "Are we cross-compiling for Windows?" FORCE)
    else()
        set(SGRN_PLATFORM_NAME "linux")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
            set(SGRN_ARCH "arm64")
        else()
            set(SGRN_ARCH "x86_64")
        endif()
        set(SGRN_CROSS_WIN64 OFF CACHE BOOL "Are we cross-compiling for Windows?" FORCE)
    endif()

    # Explicit ARM64 cross-compilation detection
    if(NOT DEFINED SGRN_CROSS_ARM64)
        if(CMAKE_CROSSCOMPILING AND SGRN_ARCH STREQUAL "arm64")
            set(SGRN_CROSS_ARM64 ON CACHE BOOL "Are we cross-compiling for ARM64?" FORCE)
        else()
            set(SGRN_CROSS_ARM64 OFF CACHE BOOL "Are we cross-compiling for ARM64?" FORCE)
        endif()
    endif()

    # Final platform identifier (e.g., windows, linux, linux-arm64)
    if(SGRN_PLATFORM_NAME STREQUAL "windows")
        set(SGRN_PLATFORM "win64") # Keeping standard win64 for windows
    else()
        if(SGRN_ARCH STREQUAL "x86_64")
            set(SGRN_PLATFORM "linux")
        else()
            set(SGRN_PLATFORM "linux-${SGRN_ARCH}")
        endif()
    endif()
endif()

message(STATUS "[SGRN] Target Platform: ${SGRN_PLATFORM}")

# 2. Paths & Repo-Local Prefix (The "Local OS")
# ------------------------------------------------------------------------------
get_filename_component(SGRN_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Export all symbols on Windows/MinGW for DLLs
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Do not force PIC/PIE globally. It makes executables build as PIE (-fPIE),
# which breaks PCH REUSE_FROM when the PCH is compiled with different PIC/PIE
# semantics. Instead, set PIC explicitly on static libs that may be linked into
# shared libraries (see cmake/pch.cmake and extern/ targets).

# The shared prefix holds architecture-neutral artifacts (headers, cmake packages
# that don't embed absolute lib paths). One copy serves all platforms.
set(SGRN_SHARED_PREFIX "${SGRN_ROOT}/.prefix" CACHE PATH "Platform-independent prefix (headers, cmake configs)")

# The local prefix holds platform-specific artifacts (static/shared libs, binaries).
# Each platform gets its own subtree: .prefix/<platform>/
set(SGRN_LOCAL_PREFIX "${SGRN_ROOT}/.prefix/${SGRN_PLATFORM}" CACHE PATH "Locally built dependencies prefix")

# Default Install Prefix:
# - Dependencies-only mode: install into .prefix/
# - Normal build: install into .dist/ (portable distribution, hidden)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(SGRN_DEPS_ONLY)
        set(CMAKE_INSTALL_PREFIX "${SGRN_LOCAL_PREFIX}" CACHE PATH "Install path prefix" FORCE)
    else()
        set(CMAKE_INSTALL_PREFIX "${SGRN_ROOT}/.dist/${SGRN_PLATFORM}" CACHE PATH "Install path prefix" FORCE)
    endif()
endif()

# Conda/Micromamba environment detection
# Priority: CMake Variable > Environment Variable > Current Conda Prefix
if(NOT SGRN_CONDA_PREFIX)
    if(SGRN_CROSS_WIN64)
        if(SGRN_WIN64_PREFIX)
            set(SGRN_CONDA_PREFIX "${SGRN_WIN64_PREFIX}")
        elseif(DEFINED ENV{SGRN_WIN64_PREFIX})
            set(SGRN_CONDA_PREFIX "$ENV{SGRN_WIN64_PREFIX}")
        endif()
    elseif(SGRN_CROSS_ARM64)
        if(SGRN_ARM64_PREFIX)
            set(SGRN_CONDA_PREFIX "${SGRN_ARM64_PREFIX}")
        elseif(DEFINED ENV{SGRN_ARM64_PREFIX})
            set(SGRN_CONDA_PREFIX "$ENV{SGRN_ARM64_PREFIX}")
        endif()
    endif()

    # Fallback to current host prefix if nothing specific set
    if(NOT SGRN_CONDA_PREFIX AND DEFINED ENV{CONDA_PREFIX})
        set(SGRN_CONDA_PREFIX "$ENV{CONDA_PREFIX}")
    endif()
    
    # Auto-detect micromamba SGRN environment if CONDA_PREFIX not set
    if(NOT SGRN_CONDA_PREFIX)
        # Try common micromamba locations
        set(_SGRN_MICROMAMBA_CANDIDATES
            "$ENV{HOME}/micromamba/envs/SGRN"
            "$ENV{HOME}/micromamba/envs/sgrn"
            "/opt/micromamba/envs/SGRN"
        )
        foreach(_candidate IN LISTS _SGRN_MICROMAMBA_CANDIDATES)
            if(EXISTS "${_candidate}/lib/cmake" OR EXISTS "${_candidate}/lib")
                set(SGRN_CONDA_PREFIX "${_candidate}")
                message(STATUS "[SGRN] Auto-detected micromamba environment: ${SGRN_CONDA_PREFIX}")
                break()
            endif()
        endforeach()
    endif()
endif()

if(SGRN_CONDA_PREFIX)
    # When cross-compiling, we must be CAREFUL not to add the host environment to CMAKE_PREFIX_PATH
    # if it conflicts with the target environment.
    # However, SGRN_CONDA_PREFIX is supposed to be the TARGET prefix here.
    list(APPEND CMAKE_PREFIX_PATH "${SGRN_CONDA_PREFIX}")
    list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
    message(STATUS "[SGRN] Target Conda Prefix: ${SGRN_CONDA_PREFIX}")
endif()

# Standardize Search Path:
# When using conda/micromamba, prioritize it over local prefix to use shared libs
# Otherwise, use local prefix first for static builds
if(SGRN_CONDA_PREFIX)
    # Use conda/micromamba libraries first, then local, then shared
    list(PREPEND CMAKE_PREFIX_PATH "${SGRN_CONDA_PREFIX}")
    if(IS_DIRECTORY "${SGRN_CONDA_PREFIX}/Library")
        list(PREPEND CMAKE_PREFIX_PATH "${SGRN_CONDA_PREFIX}/Library")
    endif()
    list(APPEND CMAKE_PREFIX_PATH "${SGRN_LOCAL_PREFIX}" "${SGRN_SHARED_PREFIX}")
else()
    # No conda: use local prefix first, then shared
    list(PREPEND CMAKE_PREFIX_PATH "${SGRN_LOCAL_PREFIX}" "${SGRN_SHARED_PREFIX}")
endif()
list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)

# 3. Language Standards
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# 4. Global Build Macros & Metadata
# ------------------------------------------------------------------------------
set(SGRN_INSTALL_TARGETS "" CACHE INTERNAL "List of targets to be installed in this run")

# All shared CMake macros live in macros.cmake (single source of truth).
include(${CMAKE_CURRENT_LIST_DIR}/macros.cmake)

# 5. Optimization & Toolchain Sanity
# ------------------------------------------------------------------------------
# Native ccache integration
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "[SGRN] ccache enabled")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
endif()

if(WIN32 OR SGRN_CROSS_WIN64)
    if(SGRN_BUILD_STATIC)
        add_link_options("-static" "-static-libgcc" "-static-libstdc++")
    endif()
    if(SGRN_USE_UCRT)
        add_compile_definitions(_UCRT)
        add_link_options("-lucrt")
    endif()
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON CACHE BOOL "Export all symbols on Windows")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Enforce global -fPIC on Linux/UNIX to align PCH compile properties with executables
    if(UNIX AND NOT APPLE)
        add_compile_options(-fPIC)
    endif()

    # Suppress noisy third-party warnings
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-literal-operator>
        -Wno-missing-field-initializers
        -Wno-missing-braces
    )

    if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
        # Enable Link-Time Optimization (IPO/LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT ipo_supported)
        if(ipo_supported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
            message(STATUS "[SGRN] Link-Time Optimization (IPO/LTO) Enabled")
        endif()

        if(NOT SGRN_CROSS_ARM64)
            add_compile_options(-O3 -march=native)
        else()
            add_compile_options(-O3)
        endif()
    endif()
endif()
