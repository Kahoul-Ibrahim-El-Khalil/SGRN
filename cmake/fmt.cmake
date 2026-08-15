# cmake/fmt.cmake — Single {fmt} resolver (deps-only AND normal builds)
# ─────────────────────────────────────────────────────────────────────────────
include_guard(GLOBAL)

# Helper: wire a fmt:: target into sgrn::fmt + extern::fmt
macro(_sgrn_use_fmt _target _header_only)
    sgrn_repair_target(${_target})
    if(NOT TARGET extern::fmt)
        add_library(extern::fmt ALIAS ${_target})
    endif()
    target_link_libraries(sgrn::fmt INTERFACE ${_target})
    if(${_header_only})
        target_compile_definitions(sgrn::fmt INTERFACE FMT_HEADER_ONLY=1)
    endif()
    set(SGRN_FMT_FROM_ENV ON)
endmacro()

# ═══════════════════════════════════════════════════════════════════════════
# Phase A — Deps-only: PRODUCE (only reached when SGRN_DEPS_ONLY=ON)
# ═══════════════════════════════════════════════════════════════════════════
if(SGRN_DEPS_ONLY)
    if(SGRN_CROSS_WIN64)
        set(FMT_INSTALL ON  CACHE BOOL "" FORCE)
        set(FMT_TEST    OFF CACHE BOOL "" FORCE)
        set(FMT_DOC     OFF CACHE BOOL "" FORCE)
        set(FMT_MODULE  OFF CACHE BOOL "" FORCE)
        set(FMT_WERROR  OFF CACHE BOOL "" FORCE)
        sgrn_fetch_source(fmt)
        add_subdirectory("${fmt_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/extern/fmt" EXCLUDE_FROM_ALL)
        if(TARGET fmt AND NOT TARGET extern::fmt)
            add_library(extern::fmt ALIAS fmt)
        endif()
        # Signal sgrn_force_build_all (in extern/CMakeLists.txt) to compile fmt
        # before `--target install` since it is EXCLUDE_FROM_ALL.
        set(SGRN_FMT_FORCE_BUILD ON)
    else()
        # linux / arm64: consumed header-only from conda at normal-build time.
        set(SGRN_FMT_FROM_ENV ON)
        message(STATUS "[SGRN/fmt] (deps) fmt resolved from target env — nothing to stage")
    endif()
    return()
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Phase B — Normal build: CONSUME
# ═══════════════════════════════════════════════════════════════════════════

# Stage 1: installed deps → use the staged fmt from extern-config.cmake
if(SGRN_USE_INSTALLED_DEPS)
    if(TARGET fmt::fmt)
        _sgrn_use_fmt(fmt::fmt FALSE)
        message(STATUS "[SGRN/fmt] Using staged fmt::fmt from ${SGRN_LOCAL_PREFIX}")
        return()
    elseif(TARGET fmt::fmt-header-only)
        _sgrn_use_fmt(fmt::fmt-header-only TRUE)
        message(STATUS "[SGRN/fmt] Using staged fmt::fmt-header-only from ${SGRN_LOCAL_PREFIX}")
        return()
    endif()
endif()

# Stage 2: win64 must always have a staged fmt — missing means deps-phase error
if(SGRN_CROSS_WIN64)
    message(FATAL_ERROR "[SGRN/fmt] No fmt target for Win64. Run the deps preset first.")
endif()

# Stage 3: discover from conda/system
find_package(fmt CONFIG QUIET)
if(NOT fmt_FOUND)
    find_package(fmt CONFIG REQUIRED)
endif()

if(TARGET fmt::fmt-header-only AND (SGRN_BUILD_STATIC OR NOT BUILD_SHARED_LIBS))
    _sgrn_use_fmt(fmt::fmt-header-only TRUE)
    message(STATUS "[SGRN/fmt] Using fmt::fmt-header-only (static build)")
elseif(TARGET fmt::fmt)
    _sgrn_use_fmt(fmt::fmt FALSE)
    message(STATUS "[SGRN/fmt] Using fmt::fmt")
elseif(TARGET fmt::fmt-header-only)
    _sgrn_use_fmt(fmt::fmt-header-only TRUE)
    message(STATUS "[SGRN/fmt] Using fmt::fmt-header-only")
else()
    message(FATAL_ERROR "[SGRN/fmt] fmt found but no usable target was exported.")
endif()
