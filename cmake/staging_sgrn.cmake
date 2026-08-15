# Staging SGRN's own libraries and headers into `.prefix/` (the sysroot):
#
#   .prefix/                     ← SGRN_SHARED_PREFIX (platform-independent)
#     include/                   ← ALL headers: third-party + SGRN (one copy)
#     lib/cmake/
#       extern/                  ← extern-config.cmake (third-party targets)
#       sgrn/                    ← sgrn-config.cmake (SGRN targets)
#
#   .prefix/<platform>/          ← SGRN_LOCAL_PREFIX (platform-specific)
#     lib/                       ← static/shared libs for this ABI
#     bin/                       ← executables for this platform
#
# After a full build + install, any platform's sysroot is:
#   include/   from SGRN_SHARED_PREFIX
#   lib/       from SGRN_LOCAL_PREFIX
# Consume with:
#   find_package(sgrn CONFIG REQUIRED)
#

include_guard(GLOBAL)

# Only run during a normal build, not during deps-only.
if(SGRN_DEPS_ONLY)
    return()
endif()

# Only install SGRN components when the user explicitly targets install.
# We piggyback on the CMake install(CODE …) mechanism.
# We also gate behind SGRN_BUILD_* options so only actually-built targets
# get staged.
# ─────────────────────────────────────────────────────────────────────────────

# ── Helper: Register an SGRN target for installation ─────────────────────────
# This is called from each component's CMakeLists.txt to register targets that
# should be installed into the prefix.
set(SGRN_STAGE_TARGETS "" CACHE INTERNAL "SGRN targets to stage into prefix" FORCE)
set(SGRN_STAGE_HEADER_DIRS "" CACHE INTERNAL "SGRN header source dirs for prefix staging" FORCE)

function(sgrn_register_stage_target target)
    list(APPEND SGRN_STAGE_TARGETS ${target})
    set(SGRN_STAGE_TARGETS "${SGRN_STAGE_TARGETS}" CACHE INTERNAL "" FORCE)
endfunction()

function(sgrn_register_stage_headers target header_source_dir header_dest_subdir)
    set_property(GLOBAL APPEND PROPERTY SGRN_STAGE_HEADER_${target} "${header_source_dir};${header_dest_subdir}")
endfunction()

# ── Install a single SGRN library target ─────────────────────────────────────
function(sgrn_stage_library target)
    if(NOT TARGET ${target})
        return()
    endif()

    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "OBJECT_LIBRARY")
        # Interface libraries have no binary, only headers.
        return()
    endif()

    install(TARGETS ${target}
        RUNTIME  DESTINATION ${SGRN_LOCAL_PREFIX}/bin
        LIBRARY  DESTINATION ${SGRN_LOCAL_PREFIX}/lib
        ARCHIVE  DESTINATION ${SGRN_LOCAL_PREFIX}/lib
    )
endfunction()

# ── Install headers for an SGRN component ────────────────────────────────────
# Headers go to SGRN_SHARED_PREFIX/include/ — shared across all platforms.
function(sgrn_stage_headers source_dir dest_subdir)
    install(DIRECTORY "${source_dir}/"
        DESTINATION "${SGRN_SHARED_PREFIX}/include/${dest_subdir}"
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
    )
endfunction()

# ── Main staging: run at end of root CMakeLists.txt ──────────────────────────
# Uses CMake install(CODE …) to write sgrn-config.cmake at install time.
# ─────────────────────────────────────────────────────────────────────────────

macro(sgrn_stage_to_prefix)
    # 1. Stage SGRN core headers
    sgrn_stage_headers("${CMAKE_SOURCE_DIR}/sgrn/core/include/sgrn" "sgrn")

    # 2. Stage SGRN utils library + headers
    if(TARGET sgrn_utils)
        sgrn_stage_library(sgrn_utils)
        sgrn_stage_headers("${CMAKE_SOURCE_DIR}/sgrn/utils/include/sgrn/utils" "sgrn/utils")
    endif()

    # 3. Stage SGRN SDK library + headers
    if(TARGET sgrn_sdk)
        sgrn_stage_library(sgrn_sdk)
        sgrn_stage_headers("${CMAKE_SOURCE_DIR}/sgrn/sdk/include/sgrn/sdk" "sgrn/sdk")
    endif()

    # 4. Stage SGRN SCL library + headers
    if(TARGET sgrn_scl)
        sgrn_stage_library(sgrn_scl)
        sgrn_stage_headers("${CMAKE_SOURCE_DIR}/sgrn/scl/include/sgrn/scl" "sgrn/scl")
    endif()

    # 5. Stage SGRN Gateway libraries + headers
    # Sub-libraries (STATIC) that compose the gateway
    foreach(_gt IN ITEMS
        sgrn_gateway_s7_wrappers
        sgrn_gateway_security
        sgrn_gateway_modbus_wrappers
        sgrn_gateway_modbus
        sgrn_gateway_ethernetip_wrappers
        sgrn_gateway_ethernetip
        sgrn_gateway_twin
        sgrn_gateway_s7
        sgrn_gateway_opcua_wrappers
        sgrn_gateway_opcua
    )
        sgrn_stage_library(${_gt})
    endforeach()

    # Main gateway shared library
    if(TARGET sgrn_gateway)
        sgrn_stage_library(sgrn_gateway)
    endif()

    # Gateway headers (all sub-libs share the same include tree)
    sgrn_stage_headers("${CMAKE_SOURCE_DIR}/sgrn/gateway/include/sgrn/gateway" "sgrn/gateway")

    # 6. Stage SGRN S7Shell library + headers
    if(TARGET sgrn_s7shell_lib)
        sgrn_stage_library(sgrn_s7shell_lib)
        sgrn_stage_headers("${CMAKE_SOURCE_DIR}/sgrn/s7shell/include/sgrn/s7shell" "sgrn/s7shell")
    endif()

    # 7. Stage SGRN Datastore libraries + headers
    # sgrn_datastore_core is INTERFACE-only (no binary); sgrn_stage_library()
    # already skips INTERFACE targets, so it is safe to include in the loop.
    foreach(_dt IN ITEMS sgrn_datastore_orm sgrn_datastore_lib)
        if(TARGET ${_dt})
            sgrn_stage_library(${_dt})
        endif()
    endforeach()
    # Stage datastore public headers
    sgrn_stage_headers("${CMAKE_SOURCE_DIR}/sgrn/datastore/include/sgrn/datastore" "sgrn/datastore")

    # 8. Stage executables
    foreach(_exe IN ITEMS gateway s7proxy mbproxy s7shell sclc sgrn_datastore)
        if(TARGET ${_exe})
            install(TARGETS ${_exe}
                RUNTIME DESTINATION ${SGRN_LOCAL_PREFIX}/bin
            )
        endif()
    endforeach()

    # ── 9. Harvest system shared libs into prefix/lib/ ──────────────────────────
    # Copy ALL runtime dependencies from conda into the prefix so the sysroot is
    # fully self-contained for both linking (-L prefix/lib) and runtime (RPATH).
    # Covers: gateway stack + datastore stack + transitive deps.
    if(SGRN_CONDA_PREFIX AND EXISTS "${SGRN_CONDA_PREFIX}/lib")
        set(_SGRN_HARVEST_PATTERNS
            # Core runtime
            "libz.so*" "libzstd.so*"
            "libssl.so*" "libcrypto.so*"
            "libsqlite3.so*"
            "libcurl.so*"
            # C++ runtime
            "libc++.so*" "libc++abi.so*"
            "libstdc++.so*" "libgcc_s.so*"
            # PostgreSQL (datastore)
            "libpq.so*" "libpqxx.so*"
            # Redis (datastore)
            "libhiredis.so*"
            # AWS SDK (datastore)
            "libaws-cpp-sdk-s3.so*" "libaws-cpp-sdk-core.so*"
            "libaws-crt-cpp.so*"
            "libaws-c-event-stream.so*" "libaws-c-common.so*"
            "libaws-c-io.so*" "libaws-c-mqtt.so*" "libaws-c-http.so*"
            "libaws-c-auth.so*" "libaws-c-cal.so*"
            "libaws-c-compression.so*" "libaws-c-sdkutils.so*"
            "libaws-checksums.so*" "libs2n.so*"
            # Compression helpers (drogon transitive)
            "libbrotlidec.so*" "libbrotlienc.so*" "libbrotlicommon.so*"
            # DNS resolution (drogon/trantor transitive)
            "libcares.so*"
            # UUID (drogon/trantor transitive)
            "libuuid.so*"
        )
        foreach(_lib_pattern IN LISTS _SGRN_HARVEST_PATTERNS)
            file(GLOB _matches "${SGRN_CONDA_PREFIX}/lib/${_lib_pattern}")
            foreach(_m IN LISTS _matches)
                install(FILES "${_m}" DESTINATION ${SGRN_LOCAL_PREFIX}/lib)
            endforeach()
        endforeach()
    endif()

    # ── 9. Generate sgrn-config.cmake package ─────────────────────────────────
    set(_sgrn_cmake_dir "lib/cmake/sgrn")

    set(SGRN_CONFIG_CONTENT [==[
# sgrn-config.cmake — Auto-generated by SGRN cmake/staging_sgrn.cmake
# Provides IMPORTED targets for all SGRN component libraries.
# Include paths resolve to the prefix/include/ tree.
cmake_minimum_required(VERSION 3.20)

get_filename_component(_SGRN_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_inc "${_SGRN_PREFIX}/include")
set(_lib "${_SGRN_PREFIX}/lib")
set(_bin "${_SGRN_PREFIX}/bin")

include(CMakeFindDependencyMacro)

# Find our third-party dependencies (needed transitively)
find_dependency(extern CONFIG REQUIRED PATHS "${_SGRN_PREFIX}/lib/cmake/extern" NO_DEFAULT_PATH)

# ── System-level dependencies (found via find_dependency) ────────────────────
# These are expected to be available in the consuming project's environment
# (conda, system packages, or bundled alongside the prefix).
find_dependency(ZLIB REQUIRED)
find_dependency(Threads REQUIRED)
find_dependency(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_dependency(CURL REQUIRED)
find_dependency(cxxopts REQUIRED)

# zstd — may not have a CMake config, provide fallback
if(NOT TARGET zstd::zstd)
    find_package(zstd QUIET CONFIG)
endif()
if(NOT TARGET zstd::zstd)
    find_package(zstd QUIET)
endif()
if(NOT TARGET zstd::zstd)
    add_library(zstd::zstd INTERFACE IMPORTED GLOBAL)
    find_library(_zstd_lib zstd)
    if(_zstd_lib)
        set_target_properties(zstd::zstd PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_zstd_lib}"
        )
    endif()
endif()

# sqlite3
if(NOT TARGET SQLite::SQLite3)
    find_package(SQLite3 QUIET)
endif()
if(NOT TARGET SQLite::SQLite3)
    add_library(SQLite::SQLite3 INTERFACE IMPORTED GLOBAL)
    find_library(_sqlite_lib sqlite3)
    if(_sqlite_lib)
        set_target_properties(SQLite::SQLite3 PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_sqlite_lib}"
        )
    endif()
endif()

# ── Wire sgrn::* aliases for system deps ─────────────────────────────────────
if(NOT TARGET sgrn::zlib)
    add_library(sgrn::zlib INTERFACE IMPORTED GLOBAL)
    target_link_libraries(sgrn::zlib INTERFACE ZLIB::ZLIB)
endif()
if(NOT TARGET sgrn::zstd)
    add_library(sgrn::zstd INTERFACE IMPORTED GLOBAL)
    target_link_libraries(sgrn::zstd INTERFACE zstd::zstd)
endif()
if(NOT TARGET sgrn::openssl)
    add_library(sgrn::openssl INTERFACE IMPORTED GLOBAL)
    target_link_libraries(sgrn::openssl INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()
if(NOT TARGET sgrn::sqlite)
    add_library(sgrn::sqlite INTERFACE IMPORTED GLOBAL)
    target_link_libraries(sgrn::sqlite INTERFACE SQLite::SQLite3)
endif()
if(NOT TARGET sgrn::cxxopts)
    add_library(sgrn::cxxopts INTERFACE IMPORTED GLOBAL)
    target_link_libraries(sgrn::cxxopts INTERFACE cxxopts::cxxopts)
endif()

function(_sgrn_lib_path _var _stem)
    foreach(_suf .so .a ".dll.a" .lib)
        foreach(_pfx lib "")
            if(EXISTS "${_lib}/${_pfx}${_stem}${_suf}")
                set(${_var} "${_lib}/${_pfx}${_stem}${_suf}")
                break()
            endif()
        endforeach()
        if(${_var})
            break()
        endif()
    endforeach()
endfunction()

# ── sgrn_core_iface ──────────────────────────────────────────────────────────
if(NOT TARGET sgrn::core)
    add_library(sgrn::core INTERFACE IMPORTED GLOBAL)
    target_include_directories(sgrn::core INTERFACE "${_inc}")
    target_link_libraries(sgrn::core INTERFACE fmt::fmt angelscript extern::angelscript_addons s7codec)
    target_compile_definitions(sgrn::core INTERFACE SGRN_NO_DROGON)
endif()

# ── sgrn_utils ───────────────────────────────────────────────────────────────
if(NOT TARGET sgrn::utils)
    _sgrn_lib_path(_utils_lib sgrn_utils)
    add_library(sgrn::utils SHARED IMPORTED GLOBAL)
    set_target_properties(sgrn::utils PROPERTIES
        IMPORTED_LOCATION "${_utils_lib}"
        IMPORTED_IMPLIB "${_utils_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}"
        INTERFACE_LINK_LIBRARIES "sgrn::core;fmt::fmt;sgrn::zlib;sgrn::zstd;sgrn::openssl"
    )
    set_target_properties(sgrn::utils PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS CPPHTTPLIB_OPENSSL_SUPPORT
    )
endif()

# ── sgrn_sdk ─────────────────────────────────────────────────────────────────
if(NOT TARGET sgrn::sdk)
    _sgrn_lib_path(_sdk_lib sgrn_sdk)
    add_library(sgrn::sdk SHARED IMPORTED GLOBAL)
    set_target_properties(sgrn::sdk PROPERTIES
        IMPORTED_LOCATION "${_sdk_lib}"
        IMPORTED_IMPLIB "${_sdk_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}"
        INTERFACE_LINK_LIBRARIES "sgrn::core;sgrn::utils;fmt::fmt"
    )
endif()

# ── sgrn_scl ─────────────────────────────────────────────────────────────────
if(NOT TARGET sgrn::scl)
    _sgrn_lib_path(_scl_lib sgrn_scl)
    add_library(sgrn::scl SHARED IMPORTED GLOBAL)
    set_target_properties(sgrn::scl PROPERTIES
        IMPORTED_LOCATION "${_scl_lib}"
        IMPORTED_IMPLIB "${_scl_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}"
        INTERFACE_LINK_LIBRARIES "sgrn::core;sgrn::utils;fmt::fmt;rapidjson;xml_h"
    )
endif()

# ── sgrn_gateway_s7_wrappers ─────────────────────────────────────────────────
if(NOT TARGET sgrn::gateway::s7::wrappers)
    _sgrn_lib_path(_gw_s7w_lib sgrn_gateway_s7_wrappers)
    add_library(sgrn::gateway::s7::wrappers STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::s7::wrappers PROPERTIES
        IMPORTED_LOCATION "${_gw_s7w_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::utils;extern::snap7cpp"
        INTERFACE_COMPILE_DEFINITIONS "SGRN_HAS_SNAP7"
    )
endif()

# ── sgrn_gateway_security ────────────────────────────────────────────────────
if(NOT TARGET sgrn::gateway::security)
    _sgrn_lib_path(_gw_sec_lib sgrn_gateway_security)
    add_library(sgrn::gateway::security STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::security PROPERTIES
        IMPORTED_LOCATION "${_gw_sec_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::scl;sgrn::utils"
    )
endif()

# ── sgrn_gateway_twin ────────────────────────────────────────────────────────
if(NOT TARGET sgrn::gateway::twin)
    _sgrn_lib_path(_gw_twin_lib sgrn_gateway_twin)
    add_library(sgrn::gateway::twin STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::twin PROPERTIES
        IMPORTED_LOCATION "${_gw_twin_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::gateway::s7::wrappers;sgrn::scl;fmt::fmt"
    )
endif()

# ── sgrn_gateway_s7 ──────────────────────────────────────────────────────────
if(NOT TARGET sgrn::gateway::s7)
    _sgrn_lib_path(_gw_s7_lib sgrn_gateway_s7)
    add_library(sgrn::gateway::s7 STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::s7 PROPERTIES
        IMPORTED_LOCATION "${_gw_s7_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::gateway::s7::wrappers;sgrn::gateway::twin;sgrn::gateway::security;sgrn::scl"
    )
endif()

# ── sgrn_gateway_modbus_wrappers ─────────────────────────────────────────────
if(NOT TARGET sgrn::gateway::modbus::wrappers)
    _sgrn_lib_path(_gw_mbw_lib sgrn_gateway_modbus_wrappers)
    add_library(sgrn::gateway::modbus::wrappers STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::modbus::wrappers PROPERTIES
        IMPORTED_LOCATION "${_gw_mbw_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::utils;extern::modbus;fmt::fmt"
    )
endif()

# ── sgrn_gateway_modbus ──────────────────────────────────────────────────────
if(NOT TARGET sgrn::gateway::modbus)
    _sgrn_lib_path(_gw_mb_lib sgrn_gateway_modbus)
    add_library(sgrn::gateway::modbus STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::modbus PROPERTIES
        IMPORTED_LOCATION "${_gw_mb_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::gateway::twin;sgrn::scl;sgrn::gateway::modbus::wrappers"
    )
endif()

# ── sgrn_gateway_ethernetip_wrappers ─────────────────────────────────────────
if(NOT TARGET sgrn::gateway::ethernetip::wrappers)
    _sgrn_lib_path(_gw_eipw_lib sgrn_gateway_ethernetip_wrappers)
    add_library(sgrn::gateway::ethernetip::wrappers STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::ethernetip::wrappers PROPERTIES
        IMPORTED_LOCATION "${_gw_eipw_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::utils;opener;fmt::fmt"
    )
endif()

# ── sgrn_gateway_ethernetip ──────────────────────────────────────────────────
if(NOT TARGET sgrn::gateway::ethernetip)
    _sgrn_lib_path(_gw_eip_lib sgrn_gateway_ethernetip)
    add_library(sgrn::gateway::ethernetip STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::ethernetip PROPERTIES
        IMPORTED_LOCATION "${_gw_eip_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::gateway::twin;sgrn::scl;sgrn::gateway::ethernetip::wrappers;s7codec"
    )
endif()

# ── sgrn_gateway_opcua_wrappers ──────────────────────────────────────────────
if(NOT TARGET sgrn::gateway::opcua::wrappers)
    _sgrn_lib_path(_gw_uaw_lib sgrn_gateway_opcua_wrappers)
    add_library(sgrn::gateway::opcua::wrappers STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::opcua::wrappers PROPERTIES
        IMPORTED_LOCATION "${_gw_uaw_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "extern::open62541;sgrn::utils"
    )
endif()

# ── sgrn_gateway_opcua ───────────────────────────────────────────────────────
if(NOT TARGET sgrn::gateway::opcua)
    _sgrn_lib_path(_gw_ua_lib sgrn_gateway_opcua)
    add_library(sgrn::gateway::opcua STATIC IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway::opcua PROPERTIES
        IMPORTED_LOCATION "${_gw_ua_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::gateway::opcua::wrappers;sgrn::gateway::twin;sgrn::gateway::security;sgrn::scl"
    )
endif()

# ── sgrn_gateway (shared) ────────────────────────────────────────────────────
if(NOT TARGET sgrn::gateway)
    _sgrn_lib_path(_gw_lib sgrn_gateway)
    add_library(sgrn::gateway SHARED IMPORTED GLOBAL)
    set_target_properties(sgrn::gateway PROPERTIES
        IMPORTED_LOCATION "${_gw_lib}"
        IMPORTED_IMPLIB "${_gw_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/sgrn/gateway"
        INTERFACE_LINK_LIBRARIES "sgrn::utils;sgrn::sdk;ixwebsocket::ixwebsocket;sgrn::gateway::s7;sgrn::gateway::twin;sgrn::gateway::opcua;sgrn::scl;sqlite_modern_cpp"
    )
endif()

# ── sgrn_s7shell_lib ─────────────────────────────────────────────────────────
if(NOT TARGET sgrn::s7shell::lib)
    _sgrn_lib_path(_s7sh_lib sgrn_s7shell_lib)
    add_library(sgrn::s7shell::lib SHARED IMPORTED GLOBAL)
    set_target_properties(sgrn::s7shell::lib PROPERTIES
        IMPORTED_LOCATION "${_s7sh_lib}"
        IMPORTED_IMPLIB "${_s7sh_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}"
        INTERFACE_LINK_LIBRARIES "sgrn::gateway::twin;sgrn::gateway::s7;sgrn::scl;angelscript;extern::angelscript_addons"
    )
endif()

# ── Convenience: executable locations ────────────────────────────────────────
set(sgrn_gateway_EXECUTABLE "${_bin}/gateway")
set(sgrn_s7proxy_EXECUTABLE   "${_bin}/s7proxy")
set(sgrn_mbproxy_EXECUTABLE   "${_bin}/mbproxy")
set(sgrn_s7shell_EXECUTABLE   "${_bin}/s7shell")
set(sgrn_sclc_EXECUTABLE      "${_bin}/sclc")

]==])

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sgrn-config.cmake" "${SGRN_CONFIG_CONTENT}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sgrn-config.cmake"
        DESTINATION "${_sgrn_cmake_dir}"
        COMPONENT sgrn
    )
endmacro()