# ── SGRN ARM64 Toolchain (Native GCC Cross-Compiler) ────────────────────────
# ─────────────────────────────────────────────────────────────────────────────

# Standard ARM64 Linux platform identity
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Force cross-compiling flag for project detection
set(SGRN_CROSS_ARM64 ON CACHE BOOL "" FORCE)

# ── Environment & Path Detection ──────────────────────────────────────────────
# Active SGRN environment provides the host compiler/toolchain.
set(HOST_CONDA_ROOT "$ENV{CONDA_PREFIX}")

if(SGRN_ARM64_PREFIX)
    # Explicit CMake preset/cache override
    set(TARGET_CONDA_ROOT "${SGRN_ARM64_PREFIX}")

elseif(DEFINED ENV{SGRN_ARM64_PREFIX})

    # Explicit environment-variable override
    set(TARGET_CONDA_ROOT "$ENV{SGRN_ARM64_PREFIX}")

elseif(EXISTS "${HOST_CONDA_ROOT}/../SGRN-ARM64")

    # Default: target environment sits next to the active SGRN environment
    set(TARGET_CONDA_ROOT "${HOST_CONDA_ROOT}/../SGRN-ARM64")

else()

    # Fallback to the active environment
    set(TARGET_CONDA_ROOT "${HOST_CONDA_ROOT}")

endif()

file(REAL_PATH "${TARGET_CONDA_ROOT}" TARGET_CONDA_ROOT)

message(STATUS "[toolchain/arm64] Host Root: ${HOST_CONDA_ROOT}")
message(STATUS "[toolchain/arm64] Target Root: ${TARGET_CONDA_ROOT}")

# Force SGRN_CONDA_PREFIX to target root for dependency discovery
set(
    SGRN_CONDA_PREFIX
    "${TARGET_CONDA_ROOT}"
    CACHE PATH
    "Conda prefix for target libraries"
    FORCE
)

# ── Cross-Compiler Selection ─────────────────────────────────────────────────
set(MIN_GNU_TARGET "aarch64-conda-linux-gnu")
set(_BIN_DIR "${HOST_CONDA_ROOT}/bin")

find_program(SGRN_C_COMPILER   NAMES ${MIN_GNU_TARGET}-gcc   PATHS ${_BIN_DIR} NO_DEFAULT_PATH)
find_program(SGRN_CXX_COMPILER NAMES ${MIN_GNU_TARGET}-g++   PATHS ${_BIN_DIR} NO_DEFAULT_PATH)
find_program(SGRN_LINKER       NAMES ${MIN_GNU_TARGET}-ld    PATHS ${_BIN_DIR} NO_DEFAULT_PATH)
find_program(SGRN_AR           NAMES ${MIN_GNU_TARGET}-ar    PATHS ${_BIN_DIR} NO_DEFAULT_PATH)
find_program(SGRN_RANLIB       NAMES ${MIN_GNU_TARGET}-ranlib PATHS ${_BIN_DIR} NO_DEFAULT_PATH)

if(NOT SGRN_C_COMPILER OR NOT SGRN_CXX_COMPILER)
    message(FATAL_ERROR "[toolchain/arm64] Could not find the GCC cross-compiler (${MIN_GNU_TARGET}-gcc).")
endif()

set(CMAKE_C_COMPILER   "${SGRN_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${SGRN_CXX_COMPILER}")
set(CMAKE_LINKER       "${SGRN_LINKER}")
set(CMAKE_AR           "${SGRN_AR}")
set(CMAKE_RANLIB       "${SGRN_RANLIB}")

# ── Build Flags ──────────────────────────────────────────────────────────────
# 1. Target Include Paths
set(SGRN_CROSS_FLAGS "-isystem ${TARGET_CONDA_ROOT}/include")

# 2. Linker Flags
set(SGRN_CROSS_LINK_FLAGS "-L${TARGET_CONDA_ROOT}/lib -Wl,-rpath-link,${TARGET_CONDA_ROOT}/lib")

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   ${SGRN_CROSS_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SGRN_CROSS_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS    "${SGRN_CROSS_LINK_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "${SGRN_CROSS_LINK_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS}")

# ── Search Paths ─────────────────────────────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH
    ${TARGET_CONDA_ROOT}
    ${HOST_CONDA_ROOT}/${MIN_GNU_TARGET}/sysroot
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    add_link_options("-s")
endif()
