cmake_minimum_required(VERSION 3.25)
project(sgrn_gateway LANGUAGES CXX)

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

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

# NOTE: protocol wrappers (s7 / modbus / ethernetip / opcua) now live in
# sgrn/wrappers/ as targets sgrn::wrappers::*, added via sgrn/CMakeLists.txt
# before this directory.

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
    PUBLIC  sgrn::common
    PUBLIC  sgrn_scl
    PUBLIC  sgrn::wrappers::modbus
    PRIVATE fmt::fmt
)

set_target_properties(sgrn_gateway_modbus PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_modbus)

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
    PUBLIC  sgrn::common
    PUBLIC  sgrn_scl
    PUBLIC  sgrn::wrappers::ethernetip
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
    src/twin/PlcMemory_io.cpp
    src/twin/PlcMemory_query.cpp
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
target_link_libraries(sgrn_gateway_twin PUBLIC sgrn::wrappers::s7 sgrn_scl fmt::fmt)
target_link_libraries(sgrn_gateway_twin PRIVATE sgrn::common)
target_link_libraries(sgrn_gateway_twin PUBLIC extern::rapidjson)
target_link_libraries(sgrn_gateway_twin PRIVATE sgrn::open62541)
target_link_libraries(sgrn_gateway_twin PUBLIC sgrn::unordered_dense)

set_target_properties(sgrn_gateway_twin PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_twin)

# ========================
# 2.42 sgrn_gateway_ports
# ========================
# Twin/security-backed implementations of the sgrn::common port interfaces.
# Constructed once by the gateway wiring and injected into the southbound
# protocol adapters (s7, modbus, ethernetip). The adapters themselves link
# only sgrn::common — never twin/security.
add_library(sgrn_gateway_ports STATIC
    src/adapters/ports/TwinPorts.cpp
)
add_library(sgrn::gateway::ports ALIAS sgrn_gateway_ports)

target_include_directories(sgrn_gateway_ports
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sgrn_gateway_ports
    PUBLIC  sgrn::common
    PRIVATE sgrn_gateway_twin
    PRIVATE sgrn_gateway_security
)

set_target_properties(sgrn_gateway_ports PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_gateway_ports)

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
    PUBLIC  sgrn::wrappers::s7
    PUBLIC  sgrn::common
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
    sgrn::wrappers::s7
    sgrn_gateway_s7
    sgrn_gateway_security
)
target_compile_definitions(sgrn_gateway_io INTERFACE SGRN_HAS_SNAP7)

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
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)
target_link_libraries(sgrn_gateway_opcua PRIVATE extern::rapidjson)

target_link_libraries(sgrn_gateway_opcua
    PUBLIC  sgrn::wrappers::opcua  # RAII types used in adapter public headers
    PUBLIC  sgrn::common           # ErrorClass used in adapter public headers
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
# Web asset generation — Svelte SPA (sgrn/web/gateway/ → dist/ → web_assets.hpp)
# ─────────────────────────────────────────────────────────────────────────────
set(WEB_SRC_DIR  "${CMAKE_SOURCE_DIR}/sgrn/web/gateway")
set(WEB_DIST_DIR "${WEB_SRC_DIR}/dist")
set(WEB_GEN_DIR  "${CMAKE_CURRENT_BINARY_DIR}/web_generated")

# ── Web UI source files tracked for rebuild ───────────────────────────────────
# Track the whole src/ tree at the new sgrn/web/gateway location (Svelte, TS,
# CSS and any future sources). CONFIGURE_DEPENDS re-globs on every build, so
# newly added or moved files are picked up automatically. Root config files are
# listed as explicit literals below: GLOB_RECURSE must NOT walk dist/ or
# node_modules/ (build artifacts would become rebuild inputs here).
file(GLOB_RECURSE WEB_SVELTE_SOURCES CONFIGURE_DEPENDS
    "${WEB_SRC_DIR}/src/*"
)

set(WEB_CONFIG_FILES
    "${WEB_SRC_DIR}/index.html"
    "${WEB_SRC_DIR}/replay.html"
    "${WEB_SRC_DIR}/vite.config.ts"
    "${WEB_SRC_DIR}/svelte.config.js"
    "${WEB_SRC_DIR}/package.json"
    "${WEB_SRC_DIR}/bun.lock"
)

# Shared TypeScript bindings used by the web app. Any change here must
# invalidate the web asset build so the embedded bundle stays in sync.
set(GATEWAY_BINDINGS_DIR "${CMAKE_SOURCE_DIR}/sgrn/typescript/gateway")
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
        ${WEB_CONFIG_FILES}
        ${WEB_GATEWAY_BINDINGS_SOURCES}
    COMMENT "[Web] Building Svelte SPA → dist/ via bun + Vite..."
    VERBATIM
)

# ── Glob the Vite dist and bake into web_assets.hpp ──────────────────────────
add_custom_command(
    OUTPUT  "${WEB_GEN_DIR}/web_assets.hpp"
    COMMAND ${Python3_EXECUTABLE}
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
    COMMAND ${Python3_EXECUTABLE}
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
    src/adapters/northbound/NorthboundServer.cpp
    src/adapters/http.cpp
    src/adapters/http/types.cpp
    src/adapters/http/path.cpp
    src/adapters/http/put_handler.cpp
    src/adapters/http/memory_handlers.cpp
    src/adapters/http/post_handler.cpp
    src/adapters/http/read_handlers.cpp
    src/adapters/http/tags_handlers.cpp
    src/adapters/http/assets.cpp
    src/adapters/s7/callbacks.cpp
    src/core/snapshot.cpp
    src/adapters/websocket/WebSocketAdapter.cpp
    src/adapters/websocket/delta_push.cpp
    src/adapters/websocket/client_messages.cpp
    src/database/GatewayDatabase.cpp
    src/database/EngineStateStore.cpp
    src/database/PersistenceService.cpp
    src/gateway.cpp
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
        extern::crow
        sgrn_gateway_s7
        sgrn_gateway_twin
        sgrn_gateway_opcua          # ← replaces inline OPC-UA sources
        sgrn_gateway_ethernetip
        sgrn_gateway_modbus
        sgrn_scl
    PRIVATE_LIBS
        sgrn::common
        sgrn_gateway_ports
    ENABLE_ORIGIN_RPATH
)

if(ASIO_FOUND)
    target_include_directories(sgrn_gateway SYSTEM PUBLIC ${ASIO_INCLUDE_DIRS})
    target_compile_definitions(sgrn_gateway PUBLIC ASIO_STANDALONE)
endif()

target_link_libraries(sgrn_gateway PUBLIC extern::sqlite_modern_cpp)
# extern::open62541 is now a transitive dependency via sgrn::wrappers::opcua;
# no need to link it directly here.

target_include_directories(sgrn_gateway PRIVATE
    ${WEB_GEN_DIR}
    ${DOC_GEN_DIR}
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    # open62541 private includes are no longer needed here — they are
    # encapsulated inside sgrn_gateway_opcua and sgrn::wrappers::opcua.
)
target_link_libraries(sgrn_gateway PRIVATE
    extern::sqlite_modern_cpp
    extern::rapidjson
    sgrn::unordered_dense
)

sgrn_target(sgrn_gateway)

# ========================
# 3. Tests
# ========================

if(SGRN_BUILD_TESTS)
    # Header-only RuntimeSync framing test. This deliberately has no Crow or
    # network dependency so malformed SGRW frames are cheap to regression-test.
    sgrn_add_component_executable(
        TARGET runtime_sync_protocol_test
        SOURCES tests/runtime_sync_protocol_test.cpp
        ENABLE_ORIGIN_RPATH
    )
    target_include_directories(runtime_sync_protocol_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME runtime_sync_protocol_test COMMAND runtime_sync_protocol_test)
    sgrn_target(runtime_sync_protocol_test)

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
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    add_test(NAME opcua_simulation_enum_projection_test COMMAND opcua_simulation_enum_projection_test)
    sgrn_target(opcua_simulation_enum_projection_test)

    # Adapter port decoupling test: drives the s7/modbus/ethernetip adapters
    # against fake sgrn::common ports. Deliberately links NO twin/security
    # library — a successful link proves the adapters are decoupled.
    sgrn_add_component_executable(
        TARGET adapter_ports_test
        SOURCES tests/adapter_ports_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_s7
            sgrn_gateway_modbus
            sgrn_gateway_ethernetip
            sgrn::common
            sgrn::open62541
            fmt::fmt
    )
    target_include_directories(adapter_ports_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    add_test(NAME adapter_ports_test COMMAND adapter_ports_test)
    sgrn_target(adapter_ports_test)

    # Twin port glue test: TwinMemoryPort against a real PlcMemory, plus
    # full classify() coverage. Links the twin on purpose (tests the glue).
    sgrn_add_component_executable(
        TARGET twin_ports_test
        SOURCES tests/twin_ports_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_ports
            sgrn_gateway_twin
            sgrn::common
    )
    target_include_directories(twin_ports_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    add_test(NAME twin_ports_test COMMAND twin_ports_test)
    sgrn_target(twin_ports_test)

    # Lost-wakeup regression test: producer threads hammer writeDbMemory()
    # while the heavy_pool dirty callback drains; every burst's final write
    # must be observed (no orphaned dirty batch). Links the twin on purpose.
    sgrn_add_component_executable(
        TARGET dirty_wakeup_test
        SOURCES tests/dirty_wakeup_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_twin
    )
    target_include_directories(dirty_wakeup_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    target_compile_definitions(dirty_wakeup_test PRIVATE ASIO_STANDALONE)
    add_test(NAME dirty_wakeup_test COMMAND dirty_wakeup_test)
    sgrn_target(dirty_wakeup_test)

    # Binary WAL regression test: multi-DB DeltaSnapshot events (db == 0,
    # dirty_dbs set) must produce anchor/delta/keyframe frames, and the
    # archive must round-trip through recoverStateFromArchives().
    sgrn_add_component_executable(
        TARGET persistence_binary_test
        SOURCES tests/persistence_binary_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway
            sgrn_gateway_twin
    )
    target_include_directories(persistence_binary_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    target_compile_definitions(persistence_binary_test PRIVATE ASIO_STANDALONE)
    add_test(NAME persistence_binary_test COMMAND persistence_binary_test)
    sgrn_target(persistence_binary_test)

    # Deterministic DB-write-ordering test: one processCommands() call over
    # scrambled DBs must invoke the batch callback in ascending DB order.
    sgrn_add_component_executable(
        TARGET process_commands_order_test
        SOURCES tests/process_commands_order_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_twin
    )
    target_include_directories(process_commands_order_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    target_compile_definitions(process_commands_order_test PRIVATE ASIO_STANDALONE)
    add_test(NAME process_commands_order_test COMMAND process_commands_order_test)
    sgrn_target(process_commands_order_test)
endif()

# 4.6 Documentation
# Documentation is now embedded in the binary instead of installed as man pages.

