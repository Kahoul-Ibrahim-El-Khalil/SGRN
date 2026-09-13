cmake_minimum_required(VERSION 3.25)
project(sgrn_s7shell LANGUAGES CXX)

find_package(Threads REQUIRED)
include(${CMAKE_SOURCE_DIR}/cmake/core.cmake)

# =============================================================================
# 1. sgrn_s7shell — S7 Shell core implementation
# =============================================================================

set(SGRN_S7SHELL_LIB_SOURCES
    src/s7shell/s7shell.cpp
    src/s7shell/print.cpp
    src/s7shell/autocomplete.cpp
    src/s7shell/schema.cpp
    src/s7shell/execute.cpp
    src/s7shell/initScript.cpp
    src/s7shell/runScript.cpp
    src/s7shell/onStart.cpp
    src/s7shell/runShellCommand.cpp
    src/SchemaVM.cpp
    src/commands/cmd_emit_as.cpp
    src/script/ScriptDataBlock.cpp
    src/script/ScriptFieldProxy.cpp
    src/script/ScriptTagTable.cpp
    src/script/ScriptPathBatch.cpp
    src/script/ScriptHexTable.cpp
    src/script/ScriptSchemaStore.cpp
    src/facades/ScriptDiagnostics.cpp
    src/facades/ScriptPlcControl.cpp
    src/facades/ScriptMemory.cpp
    src/facades/ScriptBlocks.cpp
    src/facades/ScriptAsync.cpp
    src/facades/ScriptConnectionProxy.cpp
    src/connection/ScriptS7Connection.cpp
    src/bindings/registration.cpp
    src/bindings/types.cpp
    src/bindings/globals.cpp
    src/bindings/PlcRuntime.cpp
    src/bindings/Persistence.cpp
    src/bindings/Simulation.cpp
    src/bindings/Replay.cpp
    src/bindings/Proxy.cpp
    src/bindings/GatewaySync.cpp
    src/bindings/S7Server.cpp
    src/bindings/HttpServer.cpp
    src/bindings/WebSocketServer.cpp
    src/connection/ProxySession.cpp
    src/connection/GatewaySync.cpp
    src/connection/S7ShellServer.cpp
    src/utils/json_helpers.cpp
)
if(SGRN_BUILD_OPC)
    list(APPEND SGRN_S7SHELL_LIB_SOURCES src/connection/OpcUaShellServer.cpp src/bindings/OpcUaServer.cpp)
endif()

sgrn_add_component_library(
    TARGET      sgrn_s7shell
    ALIAS       sgrn::s7shell
    TYPE        SHARED
    SOURCES     ${SGRN_S7SHELL_LIB_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC_LIBS
        sgrn_gateway
        sgrn_gateway_twin
        sgrn_gateway_s7
        sgrn_plcsim
        sgrn_scl
        sgrn_scripting
        sgrn::angelscript
        extern::angelscript_addons
        ixwebsocket::ixwebsocket
    ENABLE_ORIGIN_RPATH
)

target_include_directories(sgrn_s7shell PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
)
target_include_directories(sgrn_s7shell PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)
target_link_libraries(sgrn_s7shell PRIVATE extern::httplib)

sgrn_target(sgrn_s7shell)

if(SGRN_BUILD_OPC)
    # The OPC UA server binding reuses the gateway's OpcUaAdapter (open62541)
    # to expose the virtual PlcRuntime memory as an OPC UA address space.
    target_compile_definitions(sgrn_s7shell PUBLIC SGRN_HAS_OPC)
    target_link_libraries(sgrn_s7shell PUBLIC sgrn_gateway_opcua)
endif()

# =============================================================================
# 2. Executables have moved to sgrn/apps/ (one directory per entry point).
# =============================================================================




# =============================================================================
# 3. Tests
# =============================================================================

if(SGRN_BUILD_TESTS)
    sgrn_add_component_executable(
        TARGET wal_replayer_test
        SOURCES tests/wal_replayer_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_s7shell
            sgrn_utils
    )
    add_test(NAME wal_replayer_test COMMAND wal_replayer_test)
    target_compile_features(wal_replayer_test PRIVATE cxx_std_23)
    sgrn_target(wal_replayer_test)

    sgrn_add_component_executable(
        TARGET init_script_test
        SOURCES tests/init_script_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_s7shell
    )
    add_test(NAME init_script_test COMMAND init_script_test)
    target_compile_features(init_script_test PRIVATE cxx_std_23)
    sgrn_target(init_script_test)

    sgrn_add_component_executable(
        TARGET schema_reload_hook_test
        SOURCES tests/schema_reload_hook_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_s7shell
            sgrn_plcsim
    )
    add_test(NAME schema_reload_hook_test COMMAND schema_reload_hook_test)
    target_compile_features(schema_reload_hook_test PRIVATE cxx_std_23)
    sgrn_target(schema_reload_hook_test)

    # AngelScriptEngine concurrency smoke test: several threads drive their
    # own REPL engine through the execute() suppression paths at once.
    sgrn_add_component_executable(
        TARGET angel_concurrency_test
        SOURCES tests/angel_concurrency_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_s7shell
    )
    add_test(NAME angel_concurrency_test COMMAND angel_concurrency_test)
    target_compile_features(angel_concurrency_test PRIVATE cxx_std_23)
    sgrn_target(angel_concurrency_test)
endif()

# =============================================================================
# Packaging has moved to sgrn/apps/CMakeLists.txt (targets must exist first).
# =============================================================================
