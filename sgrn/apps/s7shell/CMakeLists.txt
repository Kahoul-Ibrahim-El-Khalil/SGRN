# s7shell — interactive S7 shell executable (sources live in sgrn/apps/s7shell/).
sgrn_add_component_executable(
    TARGET      s7shell
    SOURCES     main.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn::core
        sgrn_s7shell
        sgrn::cxxopts
    WIN_LIBS
        ${WIN_LIBS_LIST}
)
target_include_directories(s7shell PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
)
sgrn_target(s7shell)
include(s7shell.cmake)
sgrn_install_component_executable(TARGET s7shell COMPONENT s7)
