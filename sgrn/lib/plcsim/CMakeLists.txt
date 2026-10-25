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

# Store-bound coverage for the tag-table write path: buildPutItems
# (JSON + DecodedValue variants, byte-exact) and commitLocalPut's local
# shadow, loaded from the same temp-registry JSON the production loader
# parses. Pins the documented String-tag gap (no sizing branch → byte_count
# 0, writes refuse) instead of inventing registry-format semantics.
sgrn_add_component_executable(
    TARGET plc_tagtable_test
    SOURCES tests/plc_tagtable_test.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_plcsim
        sgrn::plcsim
        sgrn_scl
        sgrn_utils
        sgrn::common
        fmt::fmt
)
target_include_directories(plc_tagtable_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/common/include
)
add_test(NAME plc_tagtable_test COMMAND plc_tagtable_test)
sgrn_target(plc_tagtable_test)
