cmake_minimum_required(VERSION 3.25)
project(sgrn_wrappers LANGUAGES CXX)

include(${CMAKE_SOURCE_DIR}/cmake/core.cmake)

# Snap7 target resolution (mirrors sgrn/gateway/CMakeLists.txt)
if(TARGET extern::snap7cpp)
    set(_SNAP7_TARGET extern::snap7cpp)
elseif(TARGET snap7cpp)
    set(_SNAP7_TARGET snap7cpp)
else()
    set(_SNAP7_TARGET sgrn::snap7)
endif()

# ========================
# sgrn_wrappers_s7
# ========================
# Pure RAII C++ wrapper over Snap7 — no PlcMemory / schema deps.
set(WRAPPERS_S7_SOURCES
    src/s7/S7Client.cpp
    src/s7/S7Server.cpp
)

sgrn_add_component_library(
    TARGET      sgrn_wrappers_s7
    ALIAS       sgrn::wrappers::s7
    TYPE        STATIC
    SOURCES     ${WRAPPERS_S7_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_LIBS
        sgrn_utils
        ${_SNAP7_TARGET}
        sgrn::scl
        sgrn::open62541
    PRIVATE_LIBS
        fmt::fmt
)

# sgrn/scl/include used directly by the Snap7 wrapper headers
# (<sgrn/scl/types.hpp>, <sgrn/scl/errors.hpp>).
target_include_directories(sgrn_wrappers_s7
    PUBLIC ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)

target_compile_definitions(sgrn_wrappers_s7 PUBLIC SGRN_HAS_SNAP7)

set_target_properties(sgrn_wrappers_s7 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_wrappers_s7)

# ========================
# sgrn_wrappers_modbus
# ========================
# Pure RAII C++ wrapper over libmodbus — no PlcMemory / schema deps.
set(WRAPPERS_MODBUS_SOURCES
    src/modbus/Mapping.cpp
    src/modbus/Client.cpp
    src/modbus/Server.cpp
)

sgrn_add_component_library(
    TARGET      sgrn_wrappers_modbus
    ALIAS       sgrn::wrappers::modbus
    TYPE        STATIC
    SOURCES     ${WRAPPERS_MODBUS_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_LIBS
        sgrn_utils
    PRIVATE_LIBS
        sgrn::modbus
        fmt::fmt
)

target_link_libraries(sgrn_wrappers_modbus PRIVATE extern::modbus)

set_target_properties(sgrn_wrappers_modbus PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_wrappers_modbus)

# ========================
# sgrn_wrappers_ethernetip
# ========================
# Pure RAII C++ wrapper over OpENer — no PlcMemory / schema deps.
set(WRAPPERS_EIP_SOURCES
    src/ethernetip/Server.cpp
    src/ethernetip/Client.cpp
)

sgrn_add_component_library(
    TARGET      sgrn_wrappers_ethernetip
    ALIAS       sgrn::wrappers::ethernetip
    TYPE        STATIC
    SOURCES     ${WRAPPERS_EIP_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_LIBS
        sgrn_utils
        sgrn::opener
    PRIVATE_LIBS
        fmt::fmt
)

set_target_properties(sgrn_wrappers_ethernetip PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_wrappers_ethernetip)

# ========================
# sgrn_wrappers_opcua
# ========================
# Pure RAII C++ wrapper over the open62541 C API.
#
# Dependency rule: this target must NOT depend on sgrn_gateway_twin,
# sgrn_scl schema types, PlcMemory, or SecurityManager. Schema → UDT
# translation lives in the adapter (schema_type_registry.cpp).
#
# Headers live under:  include/sgrn/wrappers/opcua/
# Sources live under:  src/opcua/
set(WRAPPERS_OPCUA_SOURCES
    src/opcua/TypeRegistry.cpp
    src/opcua/SessionRegistry.cpp
    src/opcua/EventSupport.cpp
    src/opcua/Server.cpp
    src/opcua/Client.cpp
)

sgrn_add_component_library(
    TARGET      sgrn_wrappers_opcua
    ALIAS       sgrn::wrappers::opcua
    TYPE        STATIC
    SOURCES     ${WRAPPERS_OPCUA_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_LIBS
        extern::open62541   # UA_* types in public headers
        sgrn_utils          # sgrn::Result<>
    PRIVATE_LIBS
        fmt::fmt            # EventSupport.cpp formatting
)

target_link_libraries(sgrn_wrappers_opcua PRIVATE extern::rapidjson) # EventSupport

set_target_properties(sgrn_wrappers_opcua PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_wrappers_opcua)
