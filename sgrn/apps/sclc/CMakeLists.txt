# sclc — SCL compiler CLI (sources live in sgrn/apps/sclc/).
sgrn_add_component_executable(
    TARGET      sclc
    SOURCES     main.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_scl
        sgrn::core
        sgrn::cxxopts
)

target_include_directories(sclc PRIVATE
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)

sgrn_target(sclc)

sgrn_install_component_executable(TARGET sclc COMPONENT sclc)
