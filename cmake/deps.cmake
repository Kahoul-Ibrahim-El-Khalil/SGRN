# cmake/deps.cmake — Thin orchestrator for SGRN dependency resolution.
# =============================================================================
# Exposes stable sgrn::* INTERFACE targets. Consumers link against these only,
# shielding them from environment-specific library names/paths.
#
# Platform-specific logic is split into cmake/deps/<platform>.cmake files.
# Shared macros live in cmake/macros.cmake (included via global.cmake).
# =============================================================================
include_guard(GLOBAL)

# Macros (sgrn_alias, sgrn_link_dependency) are in cmake/macros.cmake,
# included transitively via global.cmake before this file is ever included.

# ── Helper: Target Path Repair ──────────────────────────────────────────────
include(${CMAKE_CURRENT_LIST_DIR}/fix_conda.cmake)

# ── Header-only dependencies (Vendored) ──────────────────────────────────────
include(${CMAKE_CURRENT_LIST_DIR}/deps/header_only.cmake)

# ── Threads (needed by many imported targets) ────────────────────────────────
find_package(Threads REQUIRED)
if(THREADS_FOUND AND NOT TARGET Threads::Threads)
    add_library(Threads::Threads INTERFACE IMPORTED GLOBAL)
    if(CMAKE_THREAD_LIBS_INIT)
        target_link_libraries(Threads::Threads INTERFACE "${CMAKE_THREAD_LIBS_INIT}")
    endif()
endif()

# ── Linux prefix (used by linux.cmake, aws.cmake) ───────────────────────────
if(NOT SGRN_CROSS_WIN64 AND NOT SGRN_CROSS_ARM64)
    if(SGRN_LINUX_PREFIX)
        set(LINUX_PREFIX "${SGRN_LINUX_PREFIX}" CACHE PATH "SGRN Linux dependency prefix")
    elseif(DEFINED ENV{CONDA_PREFIX})
        set(LINUX_PREFIX "$ENV{CONDA_PREFIX}" CACHE PATH "SGRN Linux dependency prefix")
    else()
        set(LINUX_PREFIX "/usr" CACHE PATH "SGRN Linux dependency prefix")
    endif()
    message(STATUS "[SGRN] Effective Linux Prefix: ${LINUX_PREFIX}")
endif()

# ── Platform dispatch ────────────────────────────────────────────────────────
if(SGRN_CROSS_WIN64)
    include(${CMAKE_CURRENT_LIST_DIR}/deps/win64.cmake)
elseif(SGRN_CROSS_ARM64)
    include(${CMAKE_CURRENT_LIST_DIR}/deps/arm64.cmake)
else()
    include(${CMAKE_CURRENT_LIST_DIR}/deps/linux.cmake)
    if(NOT SGRN_SKIP_HOST_DEPS AND SGRN_BUILD_BACKEND)
        include(${CMAKE_CURRENT_LIST_DIR}/deps/drogon.cmake)
    endif()
    if(NOT SGRN_SKIP_HOST_DEPS)
        include(${CMAKE_CURRENT_LIST_DIR}/deps/aws.cmake)
    endif()
endif()
