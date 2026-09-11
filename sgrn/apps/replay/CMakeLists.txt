# sgrn_replay — archive replay tool. Entry point lives here; replay engine
# sources stay with the gateway.
sgrn_add_component_executable(
    TARGET      sgrn_replay
    SOURCES
        main.cpp
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/src/tools/sgrn_replay.cpp
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/src/gateway.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_gateway
        sgrn_gateway_twin
        sgrn::scl
        sgrn_utils
        sgrn::zstd
        sgrn::cxxopts
        sgrn::rapidjson
        fmt::fmt
    WIN_LIBS
        ${WIN_LIBS_LIST}
)
target_include_directories(sgrn_replay PRIVATE
    ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)
sgrn_target(sgrn_replay)
sgrn_install_component_executable(TARGET sgrn_replay COMPONENT gateway)
