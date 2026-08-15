# cmake/deps/win64.cmake — Win64 (MSYS2/MinGW) manual dependency discovery
# Included by cmake/deps.cmake when SGRN_CROSS_WIN64 is ON.

if(SGRN_CONDA_PREFIX)
    message(STATUS "[SGRN/win64] Using Win64 conda prefix: ${SGRN_CONDA_PREFIX}")
endif()

# Force symbol export for shared libraries on Windows (replaces __declspec(dllexport))
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# ── Helpers ────────────────────────────────────────────────────────────────────
function(sgrn_find_win64 NAME_KEY NAMES ROOT)
    if(NOT SGRN_WIN64_${NAME_KEY}_LIB)
        set(_paths "${ROOT}/lib" "${ROOT}/Library/lib" "${ROOT}/bin" "${ROOT}/Library/bin")
        find_library(SGRN_WIN64_${NAME_KEY}_LIB
            NAMES ${NAMES}
            PATHS ${_paths}
            PATH_SUFFIXES ""
            NO_DEFAULT_PATH
        )
    endif()
endfunction()

function(sgrn_find_win64_inc NAME_KEY HEADER ROOT)
    if(NOT SGRN_WIN64_${NAME_KEY}_INC)
        set(_paths "${ROOT}/include" "${ROOT}/Library/include")
        find_path(SGRN_WIN64_${NAME_KEY}_INC
            NAMES ${HEADER}
            PATHS ${_paths}
            NO_DEFAULT_PATH)
    endif()
endfunction()

function(sgrn_ensure_target_implib _target _lib _inc)
    if(NOT TARGET ${_target})
        add_library(${_target} UNKNOWN IMPORTED GLOBAL)
        set_target_properties(${_target} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_inc}")

        # If we are in static mode, do NOT attempt to find or link against DLLs.
        if(SGRN_BUILD_STATIC)
            set_target_properties(${_target} PROPERTIES IMPORTED_LOCATION "${_lib}")
            return()
        endif()

        if(_lib MATCHES "\\.dll$")
            get_filename_component(_dir  "${_lib}" DIRECTORY)
            get_filename_component(_name "${_lib}" NAME_WLE)
            if(EXISTS "${_dir}/../lib/${_name}.lib")
                set_target_properties(${_target} PROPERTIES IMPORTED_IMPLIB "${_dir}/../lib/${_name}.lib")
            elseif(EXISTS "${_dir}/../lib/${_name}.dll.a")
                set_target_properties(${_target} PROPERTIES IMPORTED_IMPLIB "${_dir}/../lib/${_name}.dll.a")
            endif()
            set_target_properties(${_target} PROPERTIES IMPORTED_LOCATION "${_lib}")
        elseif(_lib MATCHES "\\.lib$" OR _lib MATCHES "\\.dll\\.a$")
            set_target_properties(${_target} PROPERTIES IMPORTED_IMPLIB "${_lib}")
            get_filename_component(_lib_dir  "${_lib}" DIRECTORY)
            get_filename_component(_lib_name "${_lib}" NAME_WLE)
            if(EXISTS "${_lib_dir}/${_lib_name}.dll")
                set_target_properties(${_target} PROPERTIES IMPORTED_LOCATION "${_lib_dir}/${_lib_name}.dll")
            elseif(EXISTS "${_lib_dir}/../bin/${_lib_name}.dll")
                set_target_properties(${_target} PROPERTIES IMPORTED_LOCATION "${_lib_dir}/../bin/${_lib_name}.dll")
            else()
                set_target_properties(${_target} PROPERTIES IMPORTED_LOCATION "${_lib}")
            endif()
        else()
            set_target_properties(${_target} PROPERTIES IMPORTED_LOCATION "${_lib}")
        endif()
    endif()
endfunction()

# ── 1. ZLIB ────────────────────────────────────────────────────────────────────
sgrn_find_win64(ZLIB "z;zlib" "${SGRN_WIN64_ZLIB_ROOT}")
sgrn_find_win64_inc(ZLIB "zlib.h" "${SGRN_WIN64_ZLIB_ROOT}")
if(SGRN_WIN64_ZLIB_LIB AND SGRN_WIN64_ZLIB_INC)
    message(STATUS "[SGRN/win64] zlib: ${SGRN_WIN64_ZLIB_LIB}")
    sgrn_ensure_target_implib(msys::zlib "${SGRN_WIN64_ZLIB_LIB}" "${SGRN_WIN64_ZLIB_INC}")
    target_link_libraries(sgrn::zlib INTERFACE msys::zlib)
    target_include_directories(sgrn::zlib SYSTEM INTERFACE "${SGRN_WIN64_ZLIB_INC}")
endif()

# ── 2. OpenSSL ─────────────────────────────────────────────────────────────────
sgrn_find_win64(CRYPTO "libcrypto;crypto" "${SGRN_WIN64_OPENSSL_ROOT}")
sgrn_find_win64(SSL    "libssl;ssl"       "${SGRN_WIN64_OPENSSL_ROOT}")
sgrn_find_win64_inc(OPENSSL "openssl/ssl.h" "${SGRN_WIN64_OPENSSL_ROOT}")
if(SGRN_WIN64_CRYPTO_LIB AND SGRN_WIN64_SSL_LIB AND SGRN_WIN64_OPENSSL_INC)
    message(STATUS "[SGRN/win64] crypto: ${SGRN_WIN64_CRYPTO_LIB}")
    message(STATUS "[SGRN/win64] ssl:    ${SGRN_WIN64_SSL_LIB}")
    sgrn_ensure_target_implib(msys::crypto "${SGRN_WIN64_CRYPTO_LIB}" "${SGRN_WIN64_OPENSSL_INC}")
    sgrn_ensure_target_implib(msys::ssl    "${SGRN_WIN64_SSL_LIB}"    "${SGRN_WIN64_OPENSSL_INC}")
    target_link_libraries(sgrn::openssl INTERFACE msys::ssl msys::crypto)
    target_include_directories(sgrn::openssl SYSTEM INTERFACE "${SGRN_WIN64_OPENSSL_INC}")
endif()

# ── 3. ZSTD ────────────────────────────────────────────────────────────────────
sgrn_find_win64(ZSTD "libzstd;zstd" "${SGRN_WIN64_ZSTD_ROOT}")
sgrn_find_win64_inc(ZSTD "zstd.h" "${SGRN_WIN64_ZSTD_ROOT}")
if(SGRN_WIN64_ZSTD_LIB AND SGRN_WIN64_ZSTD_INC)
    message(STATUS "[SGRN/win64] zstd: ${SGRN_WIN64_ZSTD_LIB}")
    sgrn_ensure_target_implib(msys::zstd "${SGRN_WIN64_ZSTD_LIB}" "${SGRN_WIN64_ZSTD_INC}")
    target_link_libraries(sgrn::zstd INTERFACE msys::zstd)
    target_include_directories(sgrn::zstd SYSTEM INTERFACE "${SGRN_WIN64_ZSTD_INC}")
endif()

# ── 5. SQLite ──────────────────────────────────────────────────────────────────
sgrn_find_win64(SQLITE "sqlite3" "${SGRN_WIN64_SQLITE_ROOT}")
sgrn_find_win64_inc(SQLITE "sqlite3.h" "${SGRN_WIN64_SQLITE_ROOT}")
if(SGRN_WIN64_SQLITE_LIB AND SGRN_WIN64_SQLITE_INC)
    sgrn_ensure_target_implib(msys::sqlite3 "${SGRN_WIN64_SQLITE_LIB}" "${SGRN_WIN64_SQLITE_INC}")
    target_link_libraries(sgrn::sqlite INTERFACE msys::sqlite3)
    target_include_directories(sgrn::sqlite SYSTEM INTERFACE "${SGRN_WIN64_SQLITE_INC}")
endif()

# ── 5.1 cxxopts ───────────────────────────────────────────────────────────────
sgrn_find_win64_inc(CXXOPTS "cxxopts.hpp" "${TARGET_CONDA_ROOT}")
if(SGRN_WIN64_CXXOPTS_INC)
    add_library(msys::cxxopts INTERFACE IMPORTED GLOBAL)
    set_target_properties(msys::cxxopts PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${SGRN_WIN64_CXXOPTS_INC}")
    target_link_libraries(sgrn::cxxopts INTERFACE msys::cxxopts)
endif()

# ── 6. Asio ────────────────────────────────────────────────────────────────────
sgrn_find_win64_inc(ASIO "asio.hpp" "${TARGET_CONDA_ROOT}")
if(SGRN_WIN64_ASIO_INC)
    set(ASIO_FOUND TRUE CACHE BOOL "" FORCE)
    set(ASIO_INCLUDE_DIRS ${SGRN_WIN64_ASIO_INC} CACHE PATH "" FORCE)
    add_compile_definitions(ASIO_STANDALONE)
endif()

# fmt — resolved centrally by cmake/fmt.cmake (single source of truth).
# win64 normal builds consume the staged fmt::fmt target declared by
# extern-config.cmake (Phase A of the deps phase). Never link the host fmt.
include(${CMAKE_CURRENT_LIST_DIR}/fmt.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/jsoncpp_consume.cmake)

# ── 8. open62541 (OPC UA) ──────────────────────────────────────────────────────
if(NOT TARGET extern::open62541)
    find_package(open62541 CONFIG QUIET PATHS "${SGRN_LOCAL_PREFIX}" NO_DEFAULT_PATH)
    if(TARGET open62541::open62541)
        add_library(extern::open62541 INTERFACE IMPORTED GLOBAL)
        target_link_libraries(extern::open62541 INTERFACE open62541::open62541)
    elseif(TARGET open62541)
        add_library(extern::open62541 ALIAS open62541)
    endif()
endif()

# ── 9. AngelScript ─────────────────────────────────────────────────────────────
if(NOT TARGET angelscript)
    find_package(Angelscript CONFIG QUIET PATHS "${SGRN_LOCAL_PREFIX}" NO_DEFAULT_PATH)
    if(TARGET Angelscript::angelscript)
        add_library(angelscript INTERFACE IMPORTED GLOBAL)
        target_link_libraries(angelscript INTERFACE Angelscript::angelscript)
    endif()
endif()
