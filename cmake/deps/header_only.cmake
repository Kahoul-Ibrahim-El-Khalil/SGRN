# cmake/header_only.cmake — Interface targets for header-only dependencies.
# FetchContent_Declare was called in cmake/deps/FetchDeps.cmake.
# ─────────────────────────────────────────────────────────────────────────────
include_guard(GLOBAL)

if(SGRN_USE_INSTALLED_DEPS)
    # In installed-deps mode, extern-config.cmake already declared all targets.
    # Wire sqlite_modern_cpp → sgrn::sqlite (done here since sgrn::sqlite is set up
    # after find_package(extern) returns).
    if(TARGET sqlite_modern_cpp)
        target_link_libraries(sqlite_modern_cpp INTERFACE sgrn::sqlite)
    endif()
    return()
endif()

# Helper: populate a header-only dep (no add_subdirectory).
macro(_sgrn_header_target dep_name target_name)
    if(NOT TARGET ${target_name})
        sgrn_fetch_source(${dep_name})
    endif()
endmacro()

# rapidjson
if(NOT TARGET rapidjson)
    sgrn_fetch_source(rapidjson)
    add_library(rapidjson INTERFACE)
    target_include_directories(rapidjson SYSTEM INTERFACE "${rapidjson_SOURCE_DIR}/include")
    add_library(extern::rapidjson ALIAS rapidjson)
endif()

# cpp-httplib
if(NOT TARGET httplib)
    sgrn_fetch_source(cpp_httplib)
    add_library(httplib INTERFACE)
    target_include_directories(httplib SYSTEM INTERFACE "${cpp_httplib_SOURCE_DIR}")
    target_compile_definitions(httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
    add_library(extern::httplib ALIAS httplib)
endif()

# xml_h
if(NOT TARGET xml_h)
    sgrn_fetch_source(xml_h)
    add_library(xml_h INTERFACE)
    target_include_directories(xml_h SYSTEM INTERFACE "${xml_h_SOURCE_DIR}")
    add_library(extern::xml_h ALIAS xml_h)
endif()

# sqlite_modern_cpp
if(NOT TARGET sqlite_modern_cpp)
    sgrn_fetch_source(sqlite_modern_cpp)
    add_library(sqlite_modern_cpp INTERFACE)
    target_include_directories(sqlite_modern_cpp SYSTEM INTERFACE "${sqlite_modern_cpp_SOURCE_DIR}/hdr")
    target_link_libraries(sqlite_modern_cpp INTERFACE sgrn::sqlite)
    add_library(extern::sqlite_modern_cpp ALIAS sqlite_modern_cpp)
endif()

# unordered_dense
if(NOT TARGET unordered_dense)
    sgrn_fetch_source(unordered_dense)
    add_library(unordered_dense INTERFACE)
    target_include_directories(unordered_dense SYSTEM INTERFACE "${unordered_dense_SOURCE_DIR}/include")
    add_library(extern::unordered_dense ALIAS unordered_dense)
endif()
