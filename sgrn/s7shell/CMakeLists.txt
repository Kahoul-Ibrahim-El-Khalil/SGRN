cmake_minimum_required(VERSION 3.25)
project(sgrn_s7shell LANGUAGES CXX)

find_package(Threads REQUIRED)
include(${CMAKE_SOURCE_DIR}/cmake/core.cmake)

# =============================================================================
# 1. sgrn_s7shell_lib — S7 Shell core implementation
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
    src/PlcTagTable.cpp
    src/runtime/PlcRuntime.cpp
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
    src/bindings/Proxy.cpp
    src/bindings/GatewaySync.cpp
    src/bindings/S7Server.cpp
    src/connection/ProxySession.cpp
    src/connection/GatewaySync.cpp
    src/connection/S7ShellServer.cpp
    src/utils/PlcSimClock.cpp
    src/utils/json_helpers.cpp
)
if(SGRN_BUILD_OPC)
    list(APPEND SGRN_S7SHELL_LIB_SOURCES src/connection/OpcUaShellServer.cpp src/bindings/OpcUaServer.cpp)
endif()

sgrn_add_component_library(
    TARGET      sgrn_s7shell_lib
    ALIAS       sgrn::s7shell::lib
    TYPE        SHARED
    SOURCES     ${SGRN_S7SHELL_LIB_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC_LIBS
        sgrn_gateway_twin
        sgrn_gateway_s7
        sgrn_scl
        sgrn_scripting
        sgrn::angelscript
        extern::angelscript_addons
        ixwebsocket::ixwebsocket
    ENABLE_ORIGIN_RPATH
)

target_include_directories(sgrn_s7shell_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sgrn/gateway/include
)
target_include_directories(sgrn_s7shell_lib PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/sgrn/scl/include
)
target_link_libraries(sgrn_s7shell_lib PRIVATE extern::httplib)

sgrn_target(sgrn_s7shell_lib)

if(SGRN_BUILD_OPC)
    # The OPC UA server binding reuses the gateway's OpcUaAdapter (open62541)
    # to expose the virtual PlcRuntime memory as an OPC UA address space.
    target_compile_definitions(sgrn_s7shell_lib PUBLIC SGRN_HAS_OPC)
    target_link_libraries(sgrn_s7shell_lib PUBLIC sgrn_gateway_opcua)
endif()

# =============================================================================
# 2. Executables
# =============================================================================

set(WIN_LIBS_LIST
    ws2_32 winmm rpcrt4 crypt32 advapi32 iphlpapi
    secur32 wldap32 bcrypt userenv
)

# 2.1 s7shell
sgrn_add_component_executable(
    TARGET      s7shell
    SOURCES     s7shell.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn::core
        sgrn_s7shell_lib
        sgrn::cxxopts
    WIN_LIBS
        ${WIN_LIBS_LIST}
)
target_include_directories(s7shell PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/sgrn/scl/include
    ${CMAKE_SOURCE_DIR}/sgrn/gateway/include
)
sgrn_target(s7shell)
include(s7shell.cmake)
sgrn_install_component_executable(TARGET s7shell COMPONENT s7)




# =============================================================================
# Packaging
# =============================================================================
sgrn_package_runtime_dependencies(
    APPS
        s7shell
    LIBS
        sgrn_gateway_s7_wrappers
        sgrn_gateway_s7
        sgrn_gateway_security
        sgrn_gateway_twin
        sgrn_s7shell_lib
        sgrn_scl
        sgrn_utils
    EXTERNAL_DEPS
        sgrn::snap7
        sgrn::fmt
        ixwebsocket::ixwebsocket
    COMPONENT s7shell
)
