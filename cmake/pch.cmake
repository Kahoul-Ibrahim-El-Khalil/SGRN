# cmake/pch.cmake — Centralized Tiered Precompiled Header (PCH) System
# ─────────────────────────────────────────────────────────────────────────────
# Included by: CMakeLists.txt (root), after deps are resolved.
# Activated:   only when USE_PCH=ON (default ON).
#
# Headers are expensive to compile. PCHs are organized in layers so each
# component only pays for the headers it actually uses:
#
#   sgrn_pch_std        Standard library only (STL, C headers).
#                       Linked by: all SGRN targets.
#
#   sgrn_pch_third      std + {fmt}, jsoncpp, OpenSSL, RapidJSON, httplib,
#                       AngelScript, IXWebSocket, sqlite_modern_cpp, s7codec.
#                       Linked by: utils, sdk, scl, gateway sub-targets.
#
#   sgrn_pch_s7_third   std + snap7, open62541, rapidjson, xml_h.
#                       Linked by: s7shell, gateway S7/OPC-UA targets.
#
#   sgrn_pch_net        third + Trantor + Drogon.
#                       Linked by: datastore (backend) only.
#
#   sgrn_pch_s7         third + snap7, open62541, xml_h.
#                       Linked by: gateway S7 executables.
#
# Each PCH is a small STATIC library with a dummy .cpp file and
# target_precompile_headers(). Components opt in with:
#   sgrn_use_pch(<target> <pch_target>)
# which calls target_precompile_headers(REUSE_FROM …) to share the PCH binary.
# ─────────────────────────────────────────────────────────────────────────────
include_guard(GLOBAL)

function(_ensure_pch_file filepath)
    if(NOT EXISTS "${filepath}")
        file(WRITE "${filepath}" "// Generated PCH source file\n")
    endif()
endfunction()

# ------------------------------------------------------------------------------
# 1. Base PCH: Standard Library (C + STL)
# ------------------------------------------------------------------------------
function(sgrn_create_pch_std)
    set(pch_file "${PROJECT_SOURCE_DIR}/cmake/pch_std.cpp")
    _ensure_pch_file("${pch_file}")
    add_library(sgrn_pch_std STATIC "${pch_file}")

    if(WIN32)
        target_compile_definitions(sgrn_pch_std PUBLIC _WIN32_WINNT=0x0A00)
    endif()

    # Static libs can be linked into shared libs; keep them PIC.
    set_target_properties(sgrn_pch_std PROPERTIES POSITION_INDEPENDENT_CODE ON)
    
    # Force -fPIE on Linux cross-builds to ensure PCH compatibility with executables
    if(SGRN_CROSS_ARM64 OR SGRN_CROSS_WIN64)
        target_compile_options(sgrn_pch_std PUBLIC -fPIE)
    endif()

    target_compile_features(sgrn_pch_std PUBLIC cxx_std_23)
    target_include_directories(sgrn_pch_std PUBLIC "${CMAKE_SOURCE_DIR}/sgrn/core/include")
    target_precompile_headers(sgrn_pch_std PUBLIC
        <algorithm>
        <array>
        <atomic>
        <bit>
        <cctype>
        <charconv>
        <chrono>
        <condition_variable>
        <coroutine>
        <csignal>
        <cstddef>
        <cstdint>
        <cstdlib>
        <cstring>
        <ctime>
        <exception>
        <expected>
        <filesystem>
        <format>
        <fstream>
        <functional>
        <future>
        <iomanip>
        <iostream>
        <map>
        <memory>
        <mutex>
        <optional>
        <queue>
        <random>
        <regex>
        <set>
        <sstream>
        <stdexcept>
        <string>
        <string_view>
        <thread>
        <tuple>
        <type_traits>
        <unordered_map>
        <utility>
        <variant>
        <vector>
        <any>
        <stack>
        <list>
        <deque>
        <cmath>
        <climits>
        <cassert>
        <shared_mutex>
        <semaphore>
        <latch>
        <barrier>
        <stop_token>
        <source_location>
        <span>
        <ranges>
        <compare>
        <concepts>
        <numbers>
    )
endfunction()

# ------------------------------------------------------------------------------
# 2. Third Party PCH: Base + fmt + jsoncpp + openssl
# ------------------------------------------------------------------------------
function(sgrn_create_pch_third)
    set(pch_file "${PROJECT_SOURCE_DIR}/cmake/pch_third.cpp")
    _ensure_pch_file("${pch_file}")
    add_library(sgrn_pch_third STATIC "${pch_file}")

    if(WIN32)
        target_compile_definitions(sgrn_pch_third PUBLIC _WIN32_WINNT=0x0A00)
    endif()

    set_target_properties(sgrn_pch_third PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(SGRN_CROSS_ARM64 OR SGRN_CROSS_WIN64)
        target_compile_options(sgrn_pch_third PUBLIC -fPIE)
    endif()

    target_compile_features(sgrn_pch_third PUBLIC cxx_std_23)
    target_link_libraries(sgrn_pch_third PUBLIC 
        sgrn_pch_std 
        sgrn::fmt 
        sgrn::jsoncpp 
        sgrn::openssl 
        sgrn::rapidjson 
        sgrn::httplib
        sgrn::angelscript
        sgrn::ixwebsocket
        sgrn::sqlite_modern_cpp
        sgrn::cxxopts
        sgrn::s7codec
    )
    # Include paths flow in transitively through sgrn::* target_link_libraries above.
    # No manual extern/ source paths needed (works for both build-from-source
    # and installed-deps modes that point at .prefix/<platform>/include/).
    target_precompile_headers(sgrn_pch_third PUBLIC
        <fmt/core.h> <fmt/format.h> <fmt/chrono.h> <fmt/color.h> <fmt/ostream.h> <fmt/ranges.h>
        <json/json.h> <json/value.h> <openssl/ssl.h> <openssl/crypto.h> <zstd.h> 
        <cxxopts.hpp> <sqlite_modern_cpp.h>
        <rapidjson/document.h> <rapidjson/writer.h> <rapidjson/stringbuffer.h> <rapidjson/prettywriter.h> <rapidjson/error/en.h>
        <httplib.h>
        <angelscript.h>
        <ixwebsocket/IXWebSocketServer.h> <ixwebsocket/IXWebSocket.h>
        <s7codec/s7.hpp>
    )
endfunction()

# ------------------------------------------------------------------------------
# 3. Network PCH: Third + Trantor + Drogon
# ------------------------------------------------------------------------------
function(sgrn_create_pch_net)
    set(pch_file "${PROJECT_SOURCE_DIR}/cmake/pch_net.cpp")
    _ensure_pch_file("${pch_file}")
    add_library(sgrn_pch_net STATIC "${pch_file}")

    if(WIN32)
        target_compile_definitions(sgrn_pch_net PUBLIC _WIN32_WINNT=0x0A00)
    endif()

    set_target_properties(sgrn_pch_net PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(SGRN_CROSS_ARM64 OR SGRN_CROSS_WIN64)
        target_compile_options(sgrn_pch_net PUBLIC -fPIE)
    endif()

    target_compile_features(sgrn_pch_net PUBLIC cxx_std_23)
    target_link_libraries(sgrn_pch_net PUBLIC sgrn_pch_third sgrn::trantor sgrn::drogon)
    
    # Headers live in the platform-independent shared prefix (.prefix/include/)
    target_include_directories(sgrn_pch_net SYSTEM PUBLIC
        "${SGRN_SHARED_PREFIX}/include"
    )


    target_precompile_headers(sgrn_pch_net PUBLIC
        <regex> <string> <vector> <memory>
        <trantor/utils/Logger.h> <trantor/net/EventLoop.h> <trantor/net/InetAddress.h>
        <drogon/drogon.h>
    )
endfunction()

# ------------------------------------------------------------------------------
# 4. S7 PCH: Base + Snap7 + Trantor
# ------------------------------------------------------------------------------
function(sgrn_create_pch_s7)
    set(pch_file "${PROJECT_SOURCE_DIR}/cmake/pch_s7.cpp")
    _ensure_pch_file("${pch_file}")
    add_library(sgrn_pch_s7 STATIC "${pch_file}")

    if(WIN32)
        target_compile_definitions(sgrn_pch_s7 PUBLIC _WIN32_WINNT=0x0A00)
    endif()

    set_target_properties(sgrn_pch_s7 PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(SGRN_CROSS_ARM64 OR SGRN_CROSS_WIN64)
        target_compile_options(sgrn_pch_s7 PUBLIC -fPIE)
    endif()

    target_compile_features(sgrn_pch_s7 PUBLIC cxx_std_23)
    target_link_libraries(sgrn_pch_s7 PUBLIC 
        sgrn_pch_third 
        sgrn::snap7 
        sgrn::open62541 
        sgrn::xml_h
    )

    target_include_directories(sgrn_pch_s7 PUBLIC
        "${CMAKE_SOURCE_DIR}/sgrn/core/include"
        "${CMAKE_SOURCE_DIR}/sgrn/utils/include"
    )

    target_precompile_headers(sgrn_pch_s7 PUBLIC
        <snap7.h>
        <open62541/server.h>
        <open62541/client.h>
        <xml.h>
        <sgrn/Result.hpp>
    )
endfunction()

# ------------------------------------------------------------------------------
# 5. S7 Third Party PCH: Base + RapidJSON + Open62541 + Snap7
# ------------------------------------------------------------------------------
function(sgrn_create_pch_s7_third_party)
    set(pch_file "${PROJECT_SOURCE_DIR}/cmake/pch_s7_third.cpp")
    _ensure_pch_file("${pch_file}")
    add_library(sgrn_pch_s7_third STATIC "${pch_file}")

    set_target_properties(sgrn_pch_s7_third PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(SGRN_CROSS_ARM64 OR SGRN_CROSS_WIN64)
        target_compile_options(sgrn_pch_s7_third PUBLIC -fPIE)
    endif()

    target_compile_features(sgrn_pch_s7_third PUBLIC cxx_std_23)
    
    # Include search paths flow in transitively through sgrn::* target_link_libraries.
    # Only project-own headers need explicit injection here.
    target_include_directories(sgrn_pch_s7_third PUBLIC
        "${CMAKE_SOURCE_DIR}/sgrn/core/include"
        "${CMAKE_SOURCE_DIR}/sgrn/utils/include"
    )

    target_link_libraries(sgrn_pch_s7_third PUBLIC 
        sgrn_pch_std 
        sgrn::snap7 
        sgrn::open62541 
        sgrn::rapidjson 
        sgrn::xml_h
    )

    target_precompile_headers(sgrn_pch_s7_third PUBLIC
        <snap7.h>
        <open62541/server.h>
        <open62541/client.h>
        <rapidjson/document.h>
        <rapidjson/writer.h>
        <rapidjson/stringbuffer.h>
        <xml.h>
    )
endfunction()

# ------------------------------------------------------------------------------
# Orchestration and Usage
# ------------------------------------------------------------------------------
function(sgrn_create_pch_targets)
    message(STATUS "[SGRN] Creating tiered precompiled header targets")
    sgrn_create_pch_std()
    sgrn_create_pch_third()
    sgrn_create_pch_s7_third_party()
    if(SGRN_BUILD_BACKEND)
        sgrn_create_pch_net()
    endif()
    if(SGRN_BUILD_S7)
        sgrn_create_pch_s7()
    endif()
endfunction()

macro(sgrn_use_pch target_name pch_target)
    if(USE_PCH AND TARGET ${pch_target})
        target_precompile_headers(${target_name} REUSE_FROM ${pch_target})
    endif()
endmacro()
