# cmake/deps/staging.cmake — Phase 1 Installation Pipeline
# ─────────────────────────────────────────────────────────────────────────────
# Called at the end of extern/CMakeLists.txt ONLY during SGRN_DEPS_ONLY builds
# (i.e. the `linux-deps-static` / `win64-deps-static` / `linux-arm64-deps-static`
# workflow presets). Returns immediately otherwise.
#
# Structure:
#   § 1  Install compiled libraries → SGRN_LOCAL_PREFIX/lib/     (platform-specific)
#   § 2  Install ALL headers        → SGRN_SHARED_PREFIX/include/ (architecture-neutral)
#   § 3  Generate extern-config.cmake → SGRN_LOCAL_PREFIX/lib/cmake/extern/
#
# § 2 — Header installation rules
#   Each library has one install(DIRECTORY …) block for its source-tree headers.
#   IMPORTANT: some libraries also emit GENERATED headers into the CMake build
#   tree (e.g. via GenerateExportHeader or Python code-gen scripts). These are
#   NOT captured by source-tree directory installs and must be listed with
#   explicit install(FILES …) pointing at ${CMAKE_CURRENT_BINARY_DIR}/…
#   Known generated-header locations in this build:
#     open62541: .build/…/extern/open62541/src_generated/open62541/*.h
#     trantor:   .build/…/extern/drogon/trantor/exports/trantor/exports.h
#     drogon:    .build/…/extern/drogon/exports/drogon/exports.h
#                .build/…/extern/drogon/drogon/config.h
#                .build/…/extern/drogon/lib/inc/drogon/version.h
#
# § 3 — extern-config.cmake
#   A self-contained CMake package file is generated from an inline CMake
#   string and installed to SGRN_LOCAL_PREFIX/lib/cmake/extern/.
#   It declares IMPORTED targets (extern::open62541, fmt::fmt, …) with
#   INTERFACE_INCLUDE_DIRECTORIES pointing at SGRN_SHARED_PREFIX/include/.
#   This file is the sole consumer interface for Phase 2.
#
# See BUILD_SYSTEM.md for the complete sysroot layout.
# ─────────────────────────────────────────────────────────────────────────────

if(NOT SGRN_DEPS_ONLY)
    return()
endif()

# All <dep>_SOURCE_DIR variables are set by sgrn_fetch_source() in each dep
# cmake file (via CPMAddPackage). They are in scope here since staging.cmake
# is included after all dep cmake files have run.

# s7codec is SGRN-internal: sgrn/codecs/s7codec/
get_filename_component(SGRN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
set(_s7codec_src "${SGRN_ROOT}/sgrn/codecs/s7codec/include")

# ── 1. Install libraries ──────────────────────────────────────────────────────
foreach(_t IN ITEMS
    fmt
    jsoncpp_lib
    jsoncpp_static
    snap7cpp
    snap7cpp_shared
    open62541
    angelscript
    angelscript_addons
    trantor
    drogon
    ixwebsocket
    modbus
    opener
)
    if(TARGET ${_t})
        get_target_property(_is_imported ${_t} IMPORTED)
        if(NOT _is_imported)
            set_target_properties(${_t} PROPERTIES PUBLIC_HEADER "")
            install(TARGETS ${_t}
                RUNTIME  DESTINATION bin
                LIBRARY  DESTINATION lib
                ARCHIVE  DESTINATION lib
            )
        endif()
    endif()
endforeach()

# ── 2. Install headers into prefix/include/ ───────────────────────────────────
# Headers are architecture-neutral — they live in SGRN_SHARED_PREFIX/include/
# (one copy shared by all platform builds: linux-static, win64, linux-arm64 …).
# Libs go to CMAKE_INSTALL_PREFIX/lib/ = SGRN_LOCAL_PREFIX/lib/ (platform-specific).

# Absolute destination for all header installs
if(DEFINED SGRN_SHARED_PREFIX)
    set(_INC "${SGRN_SHARED_PREFIX}/include")
else()
    # Fallback: if called without global.cmake in scope, keep old behaviour.
    set(_INC "${CMAKE_INSTALL_PREFIX}/include")
endif()

# 2a. fmt
install(DIRECTORY "${fmt_SOURCE_DIR}/include/"
    DESTINATION "${_INC}"
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# 2b. jsoncpp
install(DIRECTORY "${jsoncpp_SOURCE_DIR}/include/"
    DESTINATION "${_INC}"
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# 2c. snap7cpp — multiple source dirs flatten into include/snap7cpp/
set(_snap7_root "${snap7_SOURCE_DIR}")
install(FILES
    "${_snap7_root}/release/wrappers/c-cpp/snap7.h"
    DESTINATION "${_INC}/snap7cpp"
)
install(FILES
    "${_snap7_root}/src/sys/snap_msgsock.h"
    "${_snap7_root}/src/sys/snap_platform.h"
    "${_snap7_root}/src/sys/snap_sysutils.h"
    "${_snap7_root}/src/sys/snap_tcpsrvr.h"
    "${_snap7_root}/src/sys/snap_threads.h"
    DESTINATION "${_INC}/snap7cpp"
)
if(WIN32 OR SGRN_CROSS_WIN64)
    install(FILES "${_snap7_root}/src/sys/win_threads.h"   DESTINATION "${_INC}/snap7cpp")
else()
    install(FILES
        "${_snap7_root}/src/sys/unix_threads.h"
        "${_snap7_root}/src/sys/sol_threads.h"
        DESTINATION "${_INC}/snap7cpp"
    )
endif()

# 2d. open62541
# Install layout under ${_INC}/open62541/ so that:
#   -isystem ${_INC}/open62541   resolves  #include <open62541/server.h>
# Source structure maps as follows:
#   open62541/include/          → ${_INC}/open62541/   (contains open62541/*.h)
#   open62541/plugins/include/  → ${_INC}/open62541/   (contains open62541/plugin/*.h and open62541/*.h)
#   open62541/arch/             → ${_INC}/open62541/   (arch helper headers, e.g. ua_architecture.h)
#   open62541/deps/aa_tree.h   → ${_INC}/open62541/   (private impl included with "aa_tree.h" relative)
#   build/src_generated/open62541/ → ${_INC}/open62541/open62541/  (generated: statuscodes.h, types_generated.h, …)
if(TARGET open62541)
    # Public API headers
    install(DIRECTORY "${open62541_SOURCE_DIR}/include/"
        DESTINATION "${_INC}/open62541"
        FILES_MATCHING PATTERN "*.h"
    )
    # Plugin/default-config headers (preserve the open62541/plugin/ hierarchy inside)
    install(DIRECTORY "${open62541_SOURCE_DIR}/plugins/include/"
        DESTINATION "${_INC}/open62541"
        FILES_MATCHING PATTERN "*.h"
    )
    # Arch helper headers (ua_architecture.h etc.)
    install(DIRECTORY "${open62541_SOURCE_DIR}/arch/"
        DESTINATION "${_INC}/open62541"
        FILES_MATCHING PATTERN "*.h"
    )
    # aa_tree.h — was removed from open62541 ≥1.4 (private nodestore header).
    # Do NOT install it here; the open62541 v1.4.x tree no longer ships it.
    # All generated headers (config.h, statuscodes.h, types_generated.h, nodeids.h, …)
    install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/open62541/src_generated/open62541/"
        DESTINATION "${_INC}/open62541/open62541"
        FILES_MATCHING PATTERN "*.h"
    )
    # Patch config.h to be architecture-neutral for the shared sysroot
    install(CODE "
        set(_cfg \"\${CMAKE_INSTALL_PREFIX}/${_INC_REL}/open62541/open62541/config.h\")
        if(EXISTS \"\${_cfg}\")
            file(READ \"\${_cfg}\" _content)
            string(REGEX REPLACE \"#include \\\"[a-zA-Z0-9_]+/ua_architecture\\\\.h\\\"\" \"#if defined(UA_ARCHITECTURE_WIN32)\\n# include \\\"win32/ua_architecture.h\\\"\\n#elif defined(UA_ARCHITECTURE_POSIX)\\n# include \\\"posix/ua_architecture.h\\\"\\n#endif\" _content \"\${_content}\")
            file(WRITE \"\${_cfg}\" \"\${_content}\")
        endif()
    ")
endif()

# 2e. AngelScript core
install(DIRECTORY "${angelscript_SOURCE_DIR}/sdk/angelscript/include/"
    DESTINATION "${_INC}/angelscript"
    FILES_MATCHING PATTERN "*.h"
)

# 2f. AngelScript add-ons
install(DIRECTORY "${angelscript_SOURCE_DIR}/sdk/add_on/"
    DESTINATION "${_INC}/angelscript_addons"
    FILES_MATCHING PATTERN "*.h" PATTERN "*.cpp"
    # .cpp files are intentionally included because add-ons are header-implementation units
)

# 2g. IXWebSocket
install(DIRECTORY "${ixwebsocket_SOURCE_DIR}/ixwebsocket/"
    DESTINATION "${_INC}/ixwebsocket"
    FILES_MATCHING PATTERN "*.h"
)

# 2h. libmodbus
install(FILES
    "${libmodbus_SOURCE_DIR}/src/modbus.h"
    "${libmodbus_SOURCE_DIR}/src/modbus-rtu.h"
    "${libmodbus_SOURCE_DIR}/src/modbus-tcp.h"
    DESTINATION "${_INC}/modbus"
)
# Generated headers (modbus-version.h, config.h) that were written to build dir
set(_lm_gen "${CMAKE_CURRENT_BINARY_DIR}/libmodbus_gen")
install(FILES
    "${_lm_gen}/modbus-version.h"
    "${_lm_gen}/config.h"
    DESTINATION "${_INC}/modbus"
)

# 2i. OpENer (EtherNet/IP) — source headers + generated headers
set(_opener_src "${opener_SOURCE_DIR}/source/src")
foreach(_sub IN ITEMS "" "/cip" "/enet_encap" "/utils" "/ports")
    install(DIRECTORY "${_opener_src}${_sub}/"
        DESTINATION "${_INC}/opener${_sub}"
        FILES_MATCHING PATTERN "*.h"
        PATTERN "WIN32" EXCLUDE
    )
endforeach()
# Platform port
if(WIN32 OR SGRN_CROSS_WIN64)
    install(DIRECTORY "${_opener_src}/ports/WIN32/"
        DESTINATION "${_INC}/opener/ports/WIN32"
        FILES_MATCHING PATTERN "*.h"
    )
else()
    install(DIRECTORY "${_opener_src}/ports/POSIX/"
        DESTINATION "${_INC}/opener/ports/POSIX"
        FILES_MATCHING PATTERN "*.h"
    )
endif()
# Generated opener headers (devicedata.h, opener_user_conf.h, Ws2tcpip.h)
set(_opener_gen "${CMAKE_CURRENT_BINARY_DIR}/opener_gen")
install(DIRECTORY "${_opener_gen}/"
    DESTINATION "${_INC}/opener"
    FILES_MATCHING PATTERN "*.h"
)

# 2k. Drogon public headers
if(TARGET drogon)
    install(DIRECTORY "${drogon_SOURCE_DIR}/lib/inc/drogon/"
        DESTINATION "${_INC}/drogon"
        FILES_MATCHING PATTERN "*.h"
    )
    install(DIRECTORY "${drogon_SOURCE_DIR}/orm_lib/inc/drogon/"
        DESTINATION "${_INC}/drogon"
        FILES_MATCHING PATTERN "*.h"
    )
    install(DIRECTORY "${drogon_SOURCE_DIR}/nosql_lib/redis/inc/drogon/"
        DESTINATION "${_INC}/drogon"
        FILES_MATCHING PATTERN "*.h"
    )
    # Generated headers: exports.h, config.h, version.h live in the build tree.
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/drogon/exports/drogon/exports.h"
        DESTINATION "${_INC}/drogon"
        OPTIONAL
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/drogon/drogon/config.h"
        DESTINATION "${_INC}/drogon"
        OPTIONAL
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/drogon/lib/inc/drogon/version.h"
        DESTINATION "${_INC}/drogon"
        OPTIONAL
    )
endif()

# 2l. Trantor public headers
if(TARGET trantor)
    install(DIRECTORY "${drogon_SOURCE_DIR}/trantor/trantor/"
        DESTINATION "${_INC}/trantor"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    )
    # exports.h is generated by CMake's GenerateExportHeader into the build tree.
    # It lives at: .build/linux-deps-static/extern/drogon/trantor/exports/trantor/exports.h
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/drogon/trantor/exports/trantor/exports.h"
        DESTINATION "${_INC}/trantor"
        OPTIONAL
    )
endif()

# 2m. Header-only deps (rapidjson, cpp-httplib, xml_h, sqlite_modern_cpp, s7codec)
# These are fetched by the MAIN build via cmake/deps/header_only.cmake, not by this
# deps-only build. Guard each with an existence check: when the source is undefined
# here, install(DIRECTORY "/") would recursively copy the ENTIRE filesystem root
# into the prefix (catastrophic). The main build installs them instead.
if(EXISTS "${rapidjson_SOURCE_DIR}/include")
    install(DIRECTORY "${rapidjson_SOURCE_DIR}/include/"
        DESTINATION "${_INC}"
        FILES_MATCHING PATTERN "*.h"
    )
endif()
if(EXISTS "${cpp_httplib_SOURCE_DIR}/httplib.h")
    install(DIRECTORY "${cpp_httplib_SOURCE_DIR}/"
        DESTINATION "${_INC}/httplib"
        FILES_MATCHING PATTERN "httplib.h"
    )
endif()
if(EXISTS "${xml_h_SOURCE_DIR}")
    install(DIRECTORY "${xml_h_SOURCE_DIR}/"
        DESTINATION "${_INC}/xml_h"
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
    )
endif()
if(EXISTS "${sqlite_modern_cpp_SOURCE_DIR}/hdr")
    install(DIRECTORY "${sqlite_modern_cpp_SOURCE_DIR}/hdr/"
        DESTINATION "${_INC}"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    )
endif()
# s7codec is SGRN-internal: install from sgrn/codecs/s7codec/include/
install(DIRECTORY "${_s7codec_src}/"
    DESTINATION "${_INC}"
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# ── 3. Generate extern-config.cmake package ───────────────────────────────────
#
# We write the config file from CMake code (no .in template needed) using
# install(CODE ...). The generated file knows its own location via
# CMAKE_CURRENT_LIST_DIR so paths are relocatable.

set(_extern_cmake_dir "lib/cmake/extern")

# Determine link deps for platform-specific targets
if(WIN32 OR SGRN_CROSS_WIN64)
    set(_snap7_platform_libs  "ws2_32;winmm")
    set(_modbus_platform_libs "ws2_32")
    set(_opener_platform_libs "ws2_32;iphlpapi")
    set(_opener_restrict_c   "RESTRICT=__restrict")
    set(_as_port             "WIN32")
else()
    set(_snap7_platform_libs  "pthread;rt")
    set(_modbus_platform_libs "")
    set(_opener_platform_libs "pthread")
    set(_opener_restrict_c   "RESTRICT=__restrict__")
    set(_as_port             "POSIX")
endif()

# Compute platform suffix to pick the right port subdir for opener
if(WIN32 OR SGRN_CROSS_WIN64)
    set(_opener_port_subdir "ports/WIN32")
else()
    set(_opener_port_subdir "ports/POSIX")
endif()

# Write the config file using string(CONFIGURE) so we can substitute variables
# safely without messing up install(CODE) escaping.
set(EXTERN_CONFIG_CONTENT [==[
# extern-config.cmake — Auto-generated by SGRN extern/cmake/staging.cmake
# Provides IMPORTED targets for all vendored third-party libraries.
# Include paths resolve to the prefix/include/ tree (no extern/ source needed).
cmake_minimum_required(VERSION 3.20)

# Headers live in the shared prefix (.prefix/include), while libraries live in
# the platform-specific prefix (.prefix/<platform>/lib). Compute both paths.
get_filename_component(_EXTERN_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(_LIB_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_inc "${_EXTERN_PREFIX}/include")
set(_lib "${_LIB_PREFIX}/lib")

include(CMakeFindDependencyMacro)

function(_sgrn_static_lib _var _stem)
    foreach(_suf .a ".dll.a" .lib)
        foreach(_pfx lib "")
            if(EXISTS "${_lib}/${_pfx}${_stem}${_suf}")
                set(${_var} "${_lib}/${_pfx}${_stem}${_suf}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
    set(${_var} "" PARENT_SCOPE)
endfunction()

# ── fmt ───────────────────────────────────────────────────────────────────────
if(NOT TARGET fmt::fmt)
    _sgrn_static_lib(_fmt_lib fmt)
    if(_fmt_lib)
        add_library(fmt::fmt STATIC IMPORTED GLOBAL)
        set_target_properties(fmt::fmt PROPERTIES
            IMPORTED_LOCATION "${_fmt_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_inc}"
        )
    endif()
endif()

# ── jsoncpp ───────────────────────────────────────────────────────────────────
if(NOT TARGET JsonCpp::JsonCpp)
    _sgrn_static_lib(_jsoncpp_lib jsoncpp)
    if(NOT _jsoncpp_lib)
        _sgrn_static_lib(_jsoncpp_lib jsoncpp_lib)
    endif()
    if(_jsoncpp_lib)
        add_library(JsonCpp::JsonCpp STATIC IMPORTED GLOBAL)
        set_target_properties(JsonCpp::JsonCpp PROPERTIES
            IMPORTED_LOCATION "${_jsoncpp_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_inc}"
            INTERFACE_COMPILE_DEFINITIONS "JSONCPP_NO_STRINGVIEW"
        )
    endif()
endif()
if(TARGET JsonCpp::JsonCpp AND NOT TARGET extern::jsoncpp_lib)
    add_library(extern::jsoncpp_lib ALIAS JsonCpp::JsonCpp)
endif()

# ── snap7cpp ─────────────────────────────────────────────────────────────────
if(NOT TARGET extern::snap7cpp)
    _sgrn_static_lib(_snap7_lib snap7cpp)
    add_library(extern::snap7cpp STATIC IMPORTED GLOBAL)
    set_target_properties(extern::snap7cpp PROPERTIES
        IMPORTED_LOCATION "${_snap7_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/snap7cpp"
        INTERFACE_LINK_LIBRARIES "@_snap7_platform_libs@"
    )
endif()
if(NOT TARGET snap7cpp)
    add_library(snap7cpp ALIAS extern::snap7cpp)
endif()

# ── AngelScript core ──────────────────────────────────────────────────────────
if(NOT TARGET angelscript)
    _sgrn_static_lib(_as_lib angelscript)
    add_library(angelscript STATIC IMPORTED GLOBAL)
    set_target_properties(angelscript PROPERTIES
        IMPORTED_LOCATION "${_as_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/angelscript"
    )
endif()
if(NOT TARGET Angelscript::angelscript)
    add_library(Angelscript::angelscript ALIAS angelscript)
endif()
if(NOT TARGET extern::angelscript)
    add_library(extern::angelscript ALIAS angelscript)
endif()

# ── AngelScript add-ons ───────────────────────────────────────────────────────
if(NOT TARGET extern::angelscript_addons)
    _sgrn_static_lib(_as_addons_lib angelscript_addons)
    add_library(extern::angelscript_addons STATIC IMPORTED GLOBAL)
    set_target_properties(extern::angelscript_addons PROPERTIES
        IMPORTED_LOCATION "${_as_addons_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/angelscript_addons;${_inc}/angelscript"
        INTERFACE_LINK_LIBRARIES "angelscript"
    )
endif()
if(NOT TARGET angelscript_addons)
    add_library(angelscript_addons ALIAS extern::angelscript_addons)
endif()

# ── open62541 (OPC UA) ────────────────────────────────────────────────────────
if(NOT TARGET extern::open62541)
    _sgrn_static_lib(_ua_lib open62541)
    add_library(extern::open62541 STATIC IMPORTED GLOBAL)
    set_target_properties(extern::open62541 PROPERTIES
        IMPORTED_LOCATION "${_ua_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/open62541"
    )
endif()
if(NOT TARGET open62541)
    add_library(open62541 ALIAS extern::open62541)
endif()
if(NOT TARGET open62541::open62541)
    add_library(open62541::open62541 ALIAS extern::open62541)
endif()

# ── Drogon (backend web framework) ────────────────────────────────────────────
if(NOT TARGET drogon)
    _sgrn_static_lib(_drogon_lib drogon)
    add_library(drogon STATIC IMPORTED GLOBAL)
    set_target_properties(drogon PROPERTIES
        IMPORTED_LOCATION "${_drogon_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/drogon"
        INTERFACE_LINK_LIBRARIES "trantor"
    )
endif()
if(NOT TARGET drogon::drogon)
    add_library(drogon::drogon ALIAS drogon)
endif()
if(NOT TARGET extern::drogon)
    add_library(extern::drogon ALIAS drogon)
endif()

# ── Trantor (networking library, used by Drogon) ──────────────────────────────
if(NOT TARGET trantor)
    _sgrn_static_lib(_trantor_lib trantor)
    add_library(trantor STATIC IMPORTED GLOBAL)
    set_target_properties(trantor PROPERTIES
        IMPORTED_LOCATION "${_trantor_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/trantor"
    )
endif()
if(NOT TARGET trantor::trantor)
    add_library(trantor::trantor ALIAS trantor)
endif()
if(NOT TARGET extern::trantor)
    add_library(extern::trantor ALIAS trantor)
endif()

# ── IXWebSocket ───────────────────────────────────────────────────────────────
if(NOT TARGET ixwebsocket::ixwebsocket)
    _sgrn_static_lib(_ixws_lib ixwebsocket)
    add_library(ixwebsocket::ixwebsocket STATIC IMPORTED GLOBAL)
    set_target_properties(ixwebsocket::ixwebsocket PROPERTIES
        IMPORTED_LOCATION "${_ixws_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/ixwebsocket;${_inc}"
    )
endif()

# ── libmodbus ─────────────────────────────────────────────────────────────────
if(NOT TARGET extern::modbus)
    _sgrn_static_lib(_modbus_lib modbus)
    add_library(extern::modbus STATIC IMPORTED GLOBAL)
    set_target_properties(extern::modbus PROPERTIES
        IMPORTED_LOCATION "${_modbus_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}/modbus"
        INTERFACE_LINK_LIBRARIES "@_modbus_platform_libs@"
    )
endif()
if(NOT TARGET modbus)
    add_library(modbus ALIAS extern::modbus)
endif()

# ── OpENer (EtherNet/IP) ──────────────────────────────────────────────────────
if(NOT TARGET extern::opener)
    _sgrn_static_lib(_opener_lib opener)
    add_library(extern::opener STATIC IMPORTED GLOBAL)
    set_target_properties(extern::opener PROPERTIES
        IMPORTED_LOCATION "${_opener_lib}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${_inc}/opener;${_inc}/opener/cip;${_inc}/opener/enet_encap;${_inc}/opener/utils;${_inc}/opener/ports;${_inc}/opener/@_opener_port_subdir@"
        INTERFACE_LINK_LIBRARIES "@_opener_platform_libs@"
        INTERFACE_COMPILE_DEFINITIONS
            "PC_OPENER_ETHERNET_BUFFER_SIZE=4096;@_opener_restrict_c@"
    )
endif()
if(NOT TARGET opener)
    add_library(opener ALIAS extern::opener)
endif()

# ── Header-only: rapidjson ────────────────────────────────────────────────────
if(NOT TARGET rapidjson)
    add_library(rapidjson INTERFACE IMPORTED GLOBAL)
    target_include_directories(rapidjson INTERFACE "${_inc}")
endif()
if(NOT TARGET extern::rapidjson)
    add_library(extern::rapidjson ALIAS rapidjson)
endif()

# ── Header-only: cpp-httplib ──────────────────────────────────────────────────
if(NOT TARGET httplib)
    add_library(httplib INTERFACE IMPORTED GLOBAL)
    target_include_directories(httplib INTERFACE "${_inc}/httplib")
    target_compile_definitions(httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
endif()
if(NOT TARGET extern::httplib)
    add_library(extern::httplib ALIAS httplib)
endif()

# ── Header-only: xml_h ────────────────────────────────────────────────────────
if(NOT TARGET xml_h)
    add_library(xml_h INTERFACE IMPORTED GLOBAL)
    target_include_directories(xml_h INTERFACE "${_inc}/xml_h")
endif()
if(NOT TARGET extern::xml_h)
    add_library(extern::xml_h ALIAS xml_h)
endif()

# ── Header-only: sqlite_modern_cpp ────────────────────────────────────────────
if(NOT TARGET sqlite_modern_cpp)
    add_library(sqlite_modern_cpp INTERFACE IMPORTED GLOBAL)
    target_include_directories(sqlite_modern_cpp INTERFACE "${_inc}")
endif()
if(NOT TARGET extern::sqlite_modern_cpp)
    add_library(extern::sqlite_modern_cpp ALIAS sqlite_modern_cpp)
endif()

# ── Header-only: s7codec ─────────────────────────────────────────────────────
if(NOT TARGET s7codec)
    add_library(s7codec INTERFACE IMPORTED GLOBAL)
    target_include_directories(s7codec INTERFACE "${_inc}")
endif()
if(NOT TARGET extern::s7codec)
    add_library(extern::s7codec ALIAS s7codec)
endif()

]==])

string(CONFIGURE "${EXTERN_CONFIG_CONTENT}" EXTERN_CONFIG_CONTENT_CONFIGURED @ONLY)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extern-config.cmake" "${EXTERN_CONFIG_CONTENT_CONFIGURED}")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/extern-config.cmake"
    DESTINATION "${_extern_cmake_dir}"
    COMPONENT sgrn_extern
)
