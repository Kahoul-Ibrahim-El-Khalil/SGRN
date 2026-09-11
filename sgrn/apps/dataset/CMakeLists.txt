# sgrn_dataset — dataset extraction tool. Entry point lives here; the
# processing units stay with the gateway twin library sources.
sgrn_add_component_executable(
    TARGET      sgrn_dataset
    SOURCES
        main.cpp
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/src/tools/sgrn_dataset.cpp
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/src/tools/sgrn_dataset_transcode.cpp
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/src/tools/sgrn_dataset_merge.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_gateway_twin
        sgrn::scl
        sgrn_utils
        sgrn::common
        sgrn::zstd
        sgrn::cxxopts
        sgrn::rapidjson
        fmt::fmt
    WIN_LIBS
        ${WIN_LIBS_LIST}
)
target_include_directories(sgrn_dataset PRIVATE
    ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)
sgrn_target(sgrn_dataset)
sgrn_install_component_executable(TARGET sgrn_dataset COMPONENT gateway)
