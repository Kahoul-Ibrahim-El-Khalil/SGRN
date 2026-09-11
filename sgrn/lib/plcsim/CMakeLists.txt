cmake_minimum_required(VERSION 3.25)
project(sgrn_plcsim LANGUAGES CXX)

include(${CMAKE_SOURCE_DIR}/cmake/core.cmake)

# =============================================================================
# sgrn_plcsim — virtual-PLC data plane (runtime + simulation + replay)
# =============================================================================
# Owns schema, memory, tag table slot and DB I/O for one PLC's worth of
# memory, with simulation and WAL-replay drivers. Knows nothing about wire
# protocols or the REPL — extracted from sgrn_s7shell, which remains its
# primary consumer (script bindings, servers, shell).

set(SGRN_PLCSIM_SOURCES
    src/PlcTagTable.cpp
    src/runtime/PlcRuntime.cpp
    src/runtime/PersistenceBridge.cpp
    src/simulation/SimulationEngine.cpp
    src/replay/WalReplayer.cpp
    src/utils/PlcSimClock.cpp
)

sgrn_add_component_library(
    TARGET      sgrn_plcsim
    ALIAS       sgrn::plcsim
    TYPE        STATIC
    SOURCES     ${SGRN_PLCSIM_SOURCES}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_LIBS
        sgrn_gateway_twin
        sgrn_gateway
        sgrn_scl
    PRIVATE_LIBS
        sgrn_utils
        fmt::fmt
)

set_target_properties(sgrn_plcsim PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

sgrn_target(sgrn_plcsim)
