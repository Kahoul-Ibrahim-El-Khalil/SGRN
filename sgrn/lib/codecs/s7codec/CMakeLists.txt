project(sgrn_s7codec LANGUAGES CXX)

add_library(sgrn_s7codec INTERFACE)
add_library(sgrn::s7codec ALIAS sgrn_s7codec)

target_include_directories(sgrn_s7codec INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)


target_compile_definitions(sgrn_s7codec INTERFACE SGRN_NO_DROGON)

sgrn_install_component_library(
    TARGET sgrn_s7codec
    EXPORT_NAME sgrn_s7codec
    INCLUDE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/include
    INCLUDE_DESTINATION include
)
