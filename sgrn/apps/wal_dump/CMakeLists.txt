# sgrn_wal_dump — WAL inspection tool (sources live in sgrn/apps/wal_dump/).
sgrn_add_component_executable(
    TARGET      sgrn_wal_dump
    SOURCES     main.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_gateway_twin
        sgrn_utils
    WIN_LIBS
        ${WIN_LIBS_LIST}
)
target_include_directories(sgrn_wal_dump PRIVATE
    ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)
sgrn_target(sgrn_wal_dump)
sgrn_install_component_executable(TARGET sgrn_wal_dump COMPONENT gateway)
