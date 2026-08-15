# ── SGRN Windows Toolchain (Hybrid Native/Cross-Compile) ────────────────────────
# ─────────────────────────────────────────────────────────────────────────────

# Standard MinGW platform identity
set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Force cross-compiling flag for project detection
set(SGRN_CROSS_WIN64 ON CACHE BOOL "" FORCE)

# ── Environment & Path Detection ──────────────────────────────────────────────
# 1. HOST_ROOT: Current active Linux environment (SGRN) - provides compiler & system libs
# 2. TARGET_ROOT: Windows library environment (SGRN-WIN64) - provides project DLLs/Libs

set(HOST_CONDA_ROOT "$ENV{CONDA_PREFIX}")

if(SGRN_WIN64_PREFIX)
    set(TARGET_CONDA_ROOT "${SGRN_WIN64_PREFIX}")

elseif(DEFINED ENV{SGRN_WIN64_PREFIX})
    set(TARGET_CONDA_ROOT "$ENV{SGRN_WIN64_PREFIX}")

elseif(EXISTS "${HOST_CONDA_ROOT}/../SGRN-WIN64")
    set(TARGET_CONDA_ROOT "${HOST_CONDA_ROOT}/../SGRN-WIN64")

else()
    set(TARGET_CONDA_ROOT "${HOST_CONDA_ROOT}")
endif()

file(REAL_PATH "${TARGET_CONDA_ROOT}" TARGET_CONDA_ROOT)

message(STATUS "[toolchain/win64] Host Root: ${HOST_CONDA_ROOT}")
message(STATUS "[toolchain/win64] Target Root: ${TARGET_CONDA_ROOT}")# Force SGRN_CONDA_PREFIX to target root for dependency discovery

set(SGRN_CONDA_PREFIX "${TARGET_CONDA_ROOT}" CACHE PATH "Conda prefix for target libraries" FORCE)

# ── Host-Aware Compiler Selection ────────────────────────────────────────────
if(CMAKE_HOST_WIN32)
    set(_EXT ".exe")
    set(_BIN_DIR "/Library/bin")
    set(_NAMES_C   clang-22.exe clang.exe)
    set(_NAMES_CXX clang++-22.exe clang++.exe)
    set(_NAMES_LLD lld.exe)
    set(_SEARCH_PATHS "${TARGET_CONDA_ROOT}${_BIN_DIR}")
else()
    set(_EXT "")
    set(_BIN_DIR "/bin")
    set(_NAMES_C   clang-22 clang-19 clang)
    set(_NAMES_CXX clang++-22 clang++-19 clang++)
    set(_NAMES_LLD lld ld.lld)
    
    set(_SEARCH_PATHS "${HOST_CONDA_ROOT}${_BIN_DIR}")
    
    # ── SYSTEM SYSROOT (From Host environment's gxx_win-64) ────────────────────
    set(MIN_GNU_TARGET "x86_64-w64-mingw32")
    set(SGRN_HOST_SYSROOT "${HOST_CONDA_ROOT}/${MIN_GNU_TARGET}/sysroot")
    
    if(EXISTS "${SGRN_HOST_SYSROOT}")
        message(STATUS "[toolchain/win64] Using Host Sysroot (System Libs): ${SGRN_HOST_SYSROOT}")
        
        # ── GCC INTERNAL LIBRARIES (libstdc++, libgcc, etc) ────────────────────
        file(GLOB _GCC_DIRS "${HOST_CONDA_ROOT}/lib/gcc/${MIN_GNU_TARGET}/*")
        if(_GCC_DIRS)
            list(GET _GCC_DIRS 0 _GCC_LATEST)
            set(SGRN_GCC_LIB_PATH "${_GCC_LATEST}")
            message(STATUS "[toolchain/win64] Found GCC Libs: ${SGRN_GCC_LIB_PATH}")
        endif()

        # ── CROSS COMPILATION FLAGS ────────────────────────────────────────────
        set(SGRN_CROSS_FLAGS 
            "-target ${MIN_GNU_TARGET}"
            "--gcc-toolchain=${HOST_CONDA_ROOT}"
            "--sysroot=${SGRN_HOST_SYSROOT}"
            "-I${SGRN_GCC_LIB_PATH}/include/c++"
            "-I${SGRN_GCC_LIB_PATH}/include/c++/${MIN_GNU_TARGET}"
            "-I${SGRN_GCC_LIB_PATH}/include/c++/backward"
            "-I${SGRN_HOST_SYSROOT}/usr/include"
            "-I${HOST_CONDA_ROOT}/include"
        )
        string(REPLACE ";" " " SGRN_CROSS_FLAGS "${SGRN_CROSS_FLAGS}")

        # ── PROJECT LIBRARIES (From SGRN-WIN64) ────────────────────────────────
        set(SGRN_CROSS_LINK_FLAGS 
            "-L${TARGET_CONDA_ROOT}/Library/lib"
            "-L${TARGET_CONDA_ROOT}/Library/mingw-w64/lib"
            "-L${TARGET_CONDA_ROOT}/lib"
            "-L${SGRN_HOST_SYSROOT}/usr/lib"
            "-L${SGRN_GCC_LIB_PATH}"
        )
        string(REPLACE ";" " " SGRN_CROSS_LINK_FLAGS "${SGRN_CROSS_LINK_FLAGS}")
        set(SGRN_CROSS_LINK_FLAGS "${SGRN_CROSS_LINK_FLAGS}" CACHE STRING "Cross-compilation link flags" FORCE)
    else()
        message(WARNING "[toolchain/win64] Host Sysroot NOT found at ${SGRN_HOST_SYSROOT}. Please install gxx_win-64 in your Linux environment.")
    endif()
endif()

# Find compilers
find_program(SGRN_C_COMPILER   NAMES ${_NAMES_C}   PATHS ${_SEARCH_PATHS} NO_DEFAULT_PATH)
find_program(SGRN_CXX_COMPILER NAMES ${_NAMES_CXX} PATHS ${_SEARCH_PATHS} NO_DEFAULT_PATH)
find_program(SGRN_LINKER       NAMES ${_NAMES_LLD} PATHS ${_SEARCH_PATHS} NO_DEFAULT_PATH)

if(NOT SGRN_C_COMPILER OR NOT SGRN_CXX_COMPILER)
    message(FATAL_ERROR "[toolchain/win64] Could not find a valid Clang host compiler. Ensure you are in the SGRN (Linux) environment.")
endif()

set(CMAKE_C_COMPILER   "${SGRN_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${SGRN_CXX_COMPILER}")
set(CMAKE_LINKER       "${SGRN_LINKER}")

# ── Target Configuration ──────────────────────────────────────────────────────
set(CMAKE_C_COMPILER_TARGET   "${MIN_GNU_TARGET}")
set(CMAKE_CXX_COMPILER_TARGET "${MIN_GNU_TARGET}")

# 🎭 Force GNU personality
set(CMAKE_C_COMPILER_FRONTEND_VARIANT   GNU  CACHE INTERNAL "")
set(CMAKE_CXX_COMPILER_FRONTEND_VARIANT GNU  CACHE INTERNAL "")
set(CMAKE_C_SIMULATE_ID   "" CACHE INTERNAL "")
set(CMAKE_CXX_SIMULATE_ID "" CACHE INTERNAL "")

# MinGW + Clang: use emulated TLS so libstdc++ (built with emutls) links cleanly
# (fixes unresolved std::__once_call/std::__once_callable when using std::call_once).
set(SGRN_ISOLATION_FLAGS "-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -femulated-tls -fno-autolink -fuse-ld=lld -Wno-unused-command-line-argument -Wno-invalid-constexpr -Wno-inline-namespace-reopened-noninline -Wno-deprecated-builtins -Wno-user-defined-literals -Wno-mismatched-tags -Wno-non-virtual-dtor -Wno-unknown-attributes -Wno-unknown-warning-option -Wno-pragma-pack -Wno-macro-redefined -Wno-keyword-compat ${SGRN_CROSS_FLAGS}")

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 -DCPPHTTPLIB_WINDOWS_8_OR_LOWER_IS_NOT_SUPPORTED=0 ${SGRN_ISOLATION_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 -DCPPHTTPLIB_WINDOWS_8_OR_LOWER_IS_NOT_SUPPORTED=0 ${SGRN_ISOLATION_FLAGS}")


set(CMAKE_EXE_LINKER_FLAGS    "${SGRN_CROSS_LINK_FLAGS} ${SGRN_ISOLATION_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "${SGRN_CROSS_LINK_FLAGS} ${SGRN_ISOLATION_FLAGS} -Wl,--export-all-symbols")

# ── Search Paths ─────────────────────────────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH
    ${SGRN_HOST_SYSROOT}
    ${TARGET_CONDA_ROOT}/Library
    ${TARGET_CONDA_ROOT}
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll.a" ".a" ".lib")

# ── Dependency Hints ─────────────────────────────────────────────────────────
set(OPENSSL_ROOT_DIR "${TARGET_CONDA_ROOT}/Library")

# ── Runtime & UCRT Configuration ──────────────────────────────────────────────
if(SGRN_USE_UCRT)
    add_compile_definitions(_UCRT)
    set(WIN64_UCRT_FLAGS "-D_UCRT")
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   ${WIN64_UCRT_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${WIN64_UCRT_FLAGS}")
endif()

if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    add_link_options("-Wl,--strip-all")
endif()
