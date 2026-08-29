cmake_minimum_required(VERSION 3.25)
project(sgrn_gateway LANGUAGES CXX)

find_package(Threads REQUIRED)

include(${CMAKE_SOURCE_DIR}/cmake/core.cmake)

# Snap7 target resolution (consistent across sub-libs)
if(TARGET extern::snap7cpp)
    set(_SNAP7_TARGET extern::snap7cpp)
elseif(TARGET snap7cpp)
    set(_SNAP7_TARGET snap7cpp)
else()
    set(_SNAP7_TARGET sgrn::snap7)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# open62541 include directories — shared by the two OPC-UA targets below.
# Factored out here so neither target has to repeat the list.
# ─────────────────────────────────────────────────────────────────────────────
# (Open62541 include directories are now provided transitively by sgrn::open62541)

# ========================
# 1. sgrn_gateway_s7_wrappers
# ========================
# Pure RAII C++ wrapper over Snap7 — no PlcMemory / schema deps.
set(GATEWAY_S7_WRAPPER_SOURCES
    src/wrappers/s7/S7Client.cpp
    src/wrappers/s7/S7Server.cpp
)

add_library(sgrn_gateway_s7_wrappers STATIC ${GATEWAY_S7_WRAPPER_SOURCES})
add_library(sgrn::gateway::s7::wrappers ALIAS sgrn_gateway_s7_wrappers)

target_include_directories(sgrn_gateway_s7_wrappers
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC ${CMAKE_SOURCE_DIR}/sgrn/scl/include
)

target_link_libraries(sgrn_gateway_s7_wrappers
    PUBLIC  sgrn_utils
    PUBLIC  ${_SNAP7_TARGET}
    PUBLIC  sgrn::scl
    PUBLIC  sgrn::open62541
    PRIVATE fmt::fmt
)

target_compile_definitions(sgrn_gateway_s7_wrappers PUBLIC SGRN_HAS_SNAP7)

set_target_properties(sgrn_gateway_s7_wrappers PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_s7_wrappers)

# ========================
# 1.1 sgrn_gateway_security
# ========================
add_library(sgrn_gateway_security STATIC
    src/security/SecurityManager.cpp
    src/security/PolicyEngine.cpp
)
add_library(sgrn::gateway::security ALIAS sgrn_gateway_security)

target_include_directories(sgrn_gateway_security
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sgrn_gateway_security
    PUBLIC sgrn_scl
    PUBLIC sgrn_utils
    PUBLIC sgrn_scripting
    PUBLIC sgrn::open62541
)

set_target_properties(sgrn_gateway_security PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_security)

# ========================
# 1.45 sgrn_gateway_modbus_wrappers
# ========================
# Pure RAII C++ wrapper over libmodbus — no PlcMemory / schema deps.
set(GATEWAY_MODBUS_WRAPPER_SOURCES
    src/wrappers/modbus/Mapping.cpp
    src/wrappers/modbus/Client.cpp
    src/wrappers/modbus/Server.cpp
)

add_library(sgrn_gateway_modbus_wrappers STATIC ${GATEWAY_MODBUS_WRAPPER_SOURCES})
add_library(sgrn::gateway::modbus::wrappers ALIAS sgrn_gateway_modbus_wrappers)

target_include_directories(sgrn_gateway_modbus_wrappers
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(sgrn_gateway_modbus_wrappers PRIVATE extern::modbus)

target_link_libraries(sgrn_gateway_modbus_wrappers
    PUBLIC  sgrn_utils
    PRIVATE sgrn::modbus
    PRIVATE fmt::fmt
)

set_target_properties(sgrn_gateway_modbus_wrappers PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_modbus_wrappers)

# ========================
# 1.5 sgrn_gateway_modbus
# ========================
set(GATEWAY_MODBUS_SOURCES
    src/adapters/modbus/ModbusAdapter.cpp
    src/adapters/modbus/TypeTranslation.cpp
)

add_library(sgrn_gateway_modbus STATIC ${GATEWAY_MODBUS_SOURCES})
add_library(sgrn::gateway::modbus ALIAS sgrn_gateway_modbus)

target_include_directories(sgrn_gateway_modbus
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sgrn_gateway_modbus
    PUBLIC  sgrn_gateway_twin
    PUBLIC  sgrn_scl
    PUBLIC  sgrn_gateway_modbus_wrappers
    PUBLIC  sgrn_gateway_security
    PRIVATE fmt::fmt
)

set_target_properties(sgrn_gateway_modbus PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_modbus)

# ========================
# 1.55 sgrn_gateway_ethernetip_wrappers
# ========================
# Pure RAII C++ wrapper over OpENer — no PlcMemory / schema deps.
set(GATEWAY_EIP_WRAPPER_SOURCES
    src/wrappers/ethernetip/Server.cpp
    src/wrappers/ethernetip/Client.cpp
)

add_library(sgrn_gateway_ethernetip_wrappers STATIC ${GATEWAY_EIP_WRAPPER_SOURCES})
add_library(sgrn::gateway::ethernetip::wrappers ALIAS sgrn_gateway_ethernetip_wrappers)

target_include_directories(sgrn_gateway_ethernetip_wrappers
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sgrn_gateway_ethernetip_wrappers
    PUBLIC  sgrn_utils
    PUBLIC  sgrn::opener
    PRIVATE fmt::fmt
)

set_target_properties(sgrn_gateway_ethernetip_wrappers PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_ethernetip_wrappers)

# ========================
# 1.6 sgrn_gateway_ethernetip
# ========================
set(GATEWAY_EIP_SOURCES
    src/adapters/ethernetip/EipAdapter.cpp
    src/adapters/ethernetip/CipCodec.cpp
    src/adapters/ethernetip/TypeTranslation.cpp
)

add_library(sgrn_gateway_ethernetip STATIC ${GATEWAY_EIP_SOURCES})
add_library(sgrn::gateway::ethernetip ALIAS sgrn_gateway_ethernetip)

target_include_directories(sgrn_gateway_ethernetip
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sgrn_gateway_ethernetip
    PUBLIC  sgrn_gateway_twin
    PUBLIC  sgrn_scl
    PUBLIC  sgrn_gateway_ethernetip_wrappers
    PUBLIC  sgrn_gateway_security
    PRIVATE sgrn::s7codec
    PRIVATE fmt::fmt
)

set_target_properties(sgrn_gateway_ethernetip PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_ethernetip)

# ========================
# 2. sgrn_gateway_twin
# ========================
set(GATEWAY_TWIN_SOURCES
    src/core/snapshot.cpp
    src/core/TreeCacheEngine.cpp
    src/core/RecoveryEngine.cpp
    src/twin/DbSnapshot.cpp
    src/twin/PlcState.cpp
    src/twin/DbIOProvider.cpp
    src/twin/PlcMemory.cpp
    src/twin/encoding.cpp
    src/twin/serialization.cpp
    src/twin/path.cpp
    src/twin/TreePath.cpp
    src/twin/time_utils.cpp
    src/twin/PlcCommandProcessor.cpp
    src/twin/field_update.cpp
    src/twin/SnapshotRegistry.cpp
    src/twin/LeafDictionary.cpp
)

add_library(sgrn_gateway_twin STATIC ${GATEWAY_TWIN_SOURCES})
add_library(sgrn::gateway::twin ALIAS sgrn_gateway_twin)
target_include_directories(sgrn_gateway_twin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sgrn_gateway_twin PUBLIC sgrn_gateway_s7_wrappers sgrn_scl fmt::fmt)
target_link_libraries(sgrn_gateway_twin PUBLIC extern::rapidjson)
target_link_libraries(sgrn_gateway_twin PRIVATE sgrn::open62541)

set_target_properties(sgrn_gateway_twin PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_twin)

# ========================
# 2.45 sgrn_gateway_s7
# ========================
# S7 protocol adapter — composes Snap7 wrappers with PlcMemory and SecurityManager.
set(GATEWAY_S7_SOURCES
    src/adapters/s7/S7ProtocolAdapter.cpp
    src/adapters/s7/TypeTranslation.cpp
)

add_library(sgrn_gateway_s7 STATIC ${GATEWAY_S7_SOURCES})
add_library(sgrn::gateway::s7 ALIAS sgrn_gateway_s7)

target_include_directories(sgrn_gateway_s7
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sgrn_gateway_s7
    PUBLIC  sgrn_gateway_s7_wrappers
    PUBLIC  sgrn_gateway_twin
    PUBLIC  sgrn_gateway_security
    PUBLIC  sgrn_scl
    PRIVATE fmt::fmt
)

set_target_properties(sgrn_gateway_s7 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_s7)

# Backward-compat aggregate — prefer linking s7_wrappers / s7 / security directly.
add_library(sgrn_gateway_io INTERFACE)
add_library(sgrn::gateway::io ALIAS sgrn_gateway_io)
target_link_libraries(sgrn_gateway_io INTERFACE
    sgrn_gateway_s7_wrappers
    sgrn_gateway_s7
    sgrn_gateway_security
)
target_compile_definitions(sgrn_gateway_io INTERFACE SGRN_HAS_SNAP7)

# ========================
# 2.5 sgrn_gateway_opcua_wrappers
# ========================
# Pure RAII C++ wrapper over the open62541 C API.
#
# Dependency rule: this target must NOT depend on sgrn_gateway_twin,
# sgrn_scl schema types, PlcMemory, or SecurityManager. Schema → UDT
# translation lives in the adapter (schema_type_registry.cpp).
#
# Headers live under:  include/sgrn/gateway/wrappers/opcua/
# Sources live under:  src/wrappers/opcua/
set(GATEWAY_OPCUA_WRAPPER_SOURCES
    src/wrappers/opcua/TypeRegistry.cpp
    src/wrappers/opcua/SessionRegistry.cpp
    src/wrappers/opcua/EventSupport.cpp
    src/wrappers/opcua/Server.cpp
    src/wrappers/opcua/Client.cpp
)

add_library(sgrn_gateway_opcua_wrappers STATIC ${GATEWAY_OPCUA_WRAPPER_SOURCES})
add_library(sgrn::gateway::opcua::wrappers ALIAS sgrn_gateway_opcua_wrappers)

target_include_directories(sgrn_gateway_opcua_wrappers
    PUBLIC
        # Wrapper public headers — consumers include as:
        #   #include <sgrn/gateway/wrappers/opcua/Server.hpp>
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(sgrn_gateway_opcua_wrappers PRIVATE extern::rapidjson) # EventSupport

target_link_libraries(sgrn_gateway_opcua_wrappers
    PUBLIC  extern::open62541   # UA_* types in public headers
    PUBLIC  sgrn_utils          # sgrn::Result<>
    PRIVATE fmt::fmt            # EventSupport.cpp formatting
)

set_target_properties(sgrn_gateway_opcua_wrappers PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

# ========================
# 2.6 sgrn_gateway_opcua
# ========================
# OPC-UA adapter layer — composes the RAII wrapper with PlcMemory,
# PlcSchemaStore, SecurityManager, and TelemetryBroker.
#
# Headers live under:  include/sgrn/gateway/adapters/opcua/
# Sources live under:  src/adapters/opcua/
set(GATEWAY_OPCUA_SOURCES
    src/adapters/opcua/OpcUaAdapter.cpp
    src/adapters/opcua/schema_type_registry.cpp
    src/adapters/opcua/TypeTranslation.cpp
    src/adapters/opcua/decoders.cpp
    src/adapters/opcua/readValue.cpp
    src/adapters/opcua/writeValue.cpp
    src/adapters/opcua/delta_push.cpp
    src/adapters/opcua/node_registration.cpp
    src/adapters/opcua/information_model.cpp
    src/adapters/opcua/encoders.cpp
    # session_security.* superseded by wrappers/opcua/SessionRegistry
)

add_library(sgrn_gateway_opcua STATIC ${GATEWAY_OPCUA_SOURCES})
add_library(sgrn::gateway::opcua ALIAS sgrn_gateway_opcua)

target_include_directories(sgrn_gateway_opcua
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/scl/include
)
target_link_libraries(sgrn_gateway_opcua PRIVATE extern::rapidjson)

target_link_libraries(sgrn_gateway_opcua
    PUBLIC  sgrn_gateway_opcua_wrappers  # RAII types used in adapter public headers
    PUBLIC  sgrn_gateway_twin            # PlcMemory, TelemetryBroker
    PUBLIC  sgrn_gateway_security        # SecurityManager
    PUBLIC  sgrn_scl                     # PlcSchemaStore, S7Type
    PRIVATE sgrn::opcua_codec_table      # OPC UA <-> S7 declarative codec bridge
    PRIVATE fmt::fmt
)

set_target_properties(sgrn_gateway_opcua PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_opcua)

# ─────────────────────────────────────────────────────────────────────────────
# Web asset generation — Svelte SPA (web-svelte/ → dist/ → web_assets.hpp)
# ─────────────────────────────────────────────────────────────────────────────
set(WEB_SRC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/web-svelte")
set(WEB_DIST_DIR "${WEB_SRC_DIR}/dist")
set(WEB_GEN_DIR  "${CMAKE_CURRENT_BINARY_DIR}/web_generated")

# ── Svelte source files tracked for rebuild ───────────────────────────────────
file(GLOB_RECURSE WEB_SVELTE_SOURCES CONFIGURE_DEPENDS
    "${WEB_SRC_DIR}/src/*.svelte"
    "${WEB_SRC_DIR}/src/*.ts"
    "${WEB_SRC_DIR}/src/*.md"
    "${WEB_SRC_DIR}/index.html"
    "${WEB_SRC_DIR}/vite.config.ts"
    "${WEB_SRC_DIR}/svelte.config.js"
    "${WEB_SRC_DIR}/package.json"
)

# Shared TypeScript bindings used by the web app. Any change here must
# invalidate the web asset build so the embedded bundle stays in sync.
set(GATEWAY_BINDINGS_DIR "${CMAKE_SOURCE_DIR}/sgrn/bindings/typescript/gateway")
file(GLOB_RECURSE WEB_GATEWAY_BINDINGS_SOURCES CONFIGURE_DEPENDS
    "${GATEWAY_BINDINGS_DIR}/src/*.ts"
    "${GATEWAY_BINDINGS_DIR}/README.md"
    "${GATEWAY_BINDINGS_DIR}/package.json"
    "${GATEWAY_BINDINGS_DIR}/tsconfig.json"
)

# Sentinel file — touches after a successful Vite build
set(WEB_BUILD_SENTINEL "${WEB_DIST_DIR}/.build_done")

add_custom_command(
    OUTPUT  "${WEB_BUILD_SENTINEL}"
    COMMAND bun run build
    COMMAND ${CMAKE_COMMAND} -E touch "${WEB_BUILD_SENTINEL}"
    WORKING_DIRECTORY "${WEB_SRC_DIR}"
    DEPENDS
        ${WEB_SVELTE_SOURCES}
        ${WEB_GATEWAY_BINDINGS_SOURCES}
    COMMENT "[Web] Building Svelte SPA → dist/ via bun + Vite..."
    VERBATIM
)

# ── Glob the Vite dist and bake into web_assets.hpp ──────────────────────────
add_custom_command(
    OUTPUT  "${WEB_GEN_DIR}/web_assets.hpp"
    COMMAND python3
        "${CMAKE_SOURCE_DIR}/scripts/generate_web_headers.py"
        "${WEB_DIST_DIR}" "${WEB_GEN_DIR}"
        --level 22 --namespace "sgrn::gateway::assets::web"
    DEPENDS "${WEB_BUILD_SENTINEL}" "${CMAKE_SOURCE_DIR}/scripts/generate_web_headers.py"
    COMMENT "[Web] Compressing Svelte assets and generating C++ headers..."
    VERBATIM
)

# ─────────────────────────────────────────────────────────────────────────────
# Doc asset generation
# ─────────────────────────────────────────────────────────────────────────────
set(DOC_SRC_DIR "${CMAKE_SOURCE_DIR}/documentation/gateway")
set(DOC_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/doc_generated")

file(GLOB_RECURSE DOC_DEPS CONFIGURE_DEPENDS "${DOC_SRC_DIR}/*")

add_custom_command(
    OUTPUT "${DOC_GEN_DIR}/doc_assets.hpp"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DOC_GEN_DIR}"
    COMMAND python3
            "${CMAKE_SOURCE_DIR}/scripts/generate_embedded_assets.py"
            "${DOC_SRC_DIR}"
            "${DOC_GEN_DIR}"
            --kind "other"
            --master "doc_assets.hpp"
            --namespace "sgrn::gateway::assets::doc"
            --extensions .5 .md .txt
            --level 22
    DEPENDS
        ${DOC_DEPS}
        "${CMAKE_SOURCE_DIR}/scripts/generate_embedded_assets.py"
    COMMENT "[Gateway] Compressing documentation assets and generating C++ headers..."
    VERBATIM
)

# ========================
# 3. sgrn_gateway (shared library)
# ========================
# Orchestration layer: HTTP, WebSocket, config, database, S7 callbacks.
# OPC-UA is now a dependency, not inline source.
set(GATEWAY_LIB_SOURCES
    "${WEB_GEN_DIR}/web_assets.hpp"
    "${DOC_GEN_DIR}/doc_assets.hpp"
    src/datastore/DatastoreBridge.cpp
    src/config/gateway.cpp
    src/config/proxy.cpp
    src/config/mbproxy.cpp
    src/adapters/http.cpp
    src/adapters/http/path.cpp
    src/adapters/http/put_handler.cpp
    src/adapters/http/memory_handlers.cpp
    src/adapters/http/post_handler.cpp
    src/adapters/http/read_handlers.cpp
    src/adapters/http/assets.cpp
    src/adapters/s7/callbacks.cpp
    src/core/snapshot.cpp
    src/adapters/websocket/WebSocketAdapter.cpp
    src/database/GatewayDatabase.cpp
    src/database/EngineStateStore.cpp
    src/database/PersistenceService.cpp
)

sgrn_add_component_library(
    TARGET      sgrn_gateway
    ALIAS       sgrn::gateway
    TYPE        SHARED
    SOURCES     ${GATEWAY_LIB_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_LIBS
        sgrn_utils
        sgrn::sdk
        ixwebsocket::ixwebsocket
        sgrn_gateway_s7
        sgrn_gateway_twin
        sgrn_gateway_opcua          # ← replaces inline OPC-UA sources
        sgrn_scl
    ENABLE_ORIGIN_RPATH
)

if(ASIO_FOUND)
    target_include_directories(sgrn_gateway SYSTEM PUBLIC ${ASIO_INCLUDE_DIRS})
    target_compile_definitions(sgrn_gateway PUBLIC ASIO_STANDALONE)
endif()

target_link_libraries(sgrn_gateway PUBLIC extern::sqlite_modern_cpp)
# extern::open62541 is now a transitive dependency via sgrn_gateway_opcua_wrappers;
# no need to link it directly here.

target_include_directories(sgrn_gateway PRIVATE
    ${WEB_GEN_DIR}
    ${DOC_GEN_DIR}
    ${CMAKE_SOURCE_DIR}/sgrn/scl/include
    # open62541 private includes are no longer needed here — they are
    # encapsulated inside sgrn_gateway_opcua and sgrn_gateway_opcua_wrappers.
)
target_link_libraries(sgrn_gateway PRIVATE
    extern::sqlite_modern_cpp
    extern::httplib
    extern::rapidjson
)

sgrn_target(sgrn_gateway)

# ========================
# 3. Tests
# ========================

if(SGRN_BUILD_TESTS)
    sgrn_add_component_executable(
        TARGET opcua_simulation_enum_projection_test
        SOURCES tests/opcua_simulation_enum_projection_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_opcua
            sgrn_scl
    )
    target_include_directories(opcua_simulation_enum_projection_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/sgrn/scl/include
    )
    add_test(NAME opcua_simulation_enum_projection_test COMMAND opcua_simulation_enum_projection_test)
    sgrn_target(opcua_simulation_enum_projection_test)
endif()

# ========================
# 4. Executables
# ========================

set(WIN_LIBS_LIST
    ws2_32 winmm rpcrt4 crypt32 advapi32 iphlpapi
    secur32 wldap32 bcrypt userenv
)

# 4.1 gateway
sgrn_add_component_executable(
    TARGET      gateway
    SOURCES     apps/gateway.cpp
    PCH         sgrn_pch_s7
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_gateway
        sgrn_gateway_modbus
        sgrn_gateway_ethernetip
        Threads::Threads
        sgrn::zstd sgrn::openssl
    WIN_LIBS
        ${WIN_LIBS_LIST}
)
target_include_directories(gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sgrn/scl/include
)
sgrn_target(gateway)
sgrn_install_component_executable(TARGET gateway COMPONENT gateway)

# 4.2 mbproxy
sgrn_add_component_executable(
    TARGET      mbproxy
    SOURCES     apps/mbproxy.cpp
    PCH         sgrn_pch_s7
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_gateway
        sgrn_gateway_modbus_wrappers
        Threads::Threads
    WIN_LIBS
        ${WIN_LIBS_LIST}
)
target_include_directories(mbproxy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sgrn/scl/include
)
sgrn_target(mbproxy)
sgrn_install_component_executable(TARGET mbproxy COMPONENT gateway)

# 4.3 sgrn-wal-dump
sgrn_add_component_executable(
    TARGET      sgrn_wal_dump
    SOURCES     tools/wal_dump/main.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_gateway_twin
        sgrn_utils
    WIN_LIBS
        ${WIN_LIBS_LIST}
)
target_include_directories(sgrn_wal_dump PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sgrn/scl/include
)
sgrn_target(sgrn_wal_dump)
sgrn_install_component_executable(TARGET sgrn_wal_dump COMPONENT gateway)

# 4.4 Documentation
# Documentation is now embedded in the binary instead of installed as man pages.

# Packaging
sgrn_package_runtime_dependencies(
    APPS
        gateway
        mbproxy
    LIBS
        sgrn_gateway
    COMPONENT gateway
)
