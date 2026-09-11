# mbproxy — Modbus proxy executable (sources live in sgrn/apps/mbproxy/).
sgrn_add_component_executable(
    TARGET      mbproxy
    SOURCES     main.cpp
    PCH         sgrn_pch_s7
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_gateway
        sgrn::wrappers::modbus
        Threads::Threads
    WIN_LIBS
        ${WIN_LIBS_LIST}
)
target_include_directories(mbproxy PRIVATE
    ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)
sgrn_target(mbproxy)
sgrn_install_component_executable(TARGET mbproxy COMPONENT gateway)
