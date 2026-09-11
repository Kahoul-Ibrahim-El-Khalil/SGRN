# gateway — main SGRN gateway executable (sources live in sgrn/apps/gateway/).
sgrn_add_component_executable(
    TARGET      gateway
    SOURCES     main.cpp
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
    ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)
sgrn_target(gateway)
sgrn_install_component_executable(TARGET gateway COMPONENT gateway)
