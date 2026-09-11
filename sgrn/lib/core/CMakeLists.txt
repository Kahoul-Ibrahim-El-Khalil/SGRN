project(sgrn_core LANGUAGES CXX)

add_library(sgrn_core INTERFACE)
add_library(sgrn::core ALIAS sgrn_core)

target_include_directories(sgrn_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Core interface dependencies — include dirs propagate via the targets
target_link_libraries(sgrn_core INTERFACE
    sgrn::fmt
    sgrn::angelscript
    sgrn::angelscript_addons
    sgrn::s7codec
    extern::rapidjson
)

target_compile_definitions(sgrn_core INTERFACE SGRN_NO_DROGON)

sgrn_install_component_library(
    TARGET sgrn_core
    EXPORT_NAME sgrn_core
    INCLUDE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/include/sgrn
    INCLUDE_DESTINATION include
)
