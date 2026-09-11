project(sgrn_common LANGUAGES CXX)

add_library(sgrn_common INTERFACE)
add_library(sgrn::common ALIAS sgrn_common)

target_include_directories(sgrn_common INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Header-only protocol-independent utilities (path/JSON/schema helpers,
# S7 serialization, endian helpers). Gateway-coupled helpers
# (AdapterBase, SecurityHelper, event_helper, SchemaResolver, ErrorClass)
# stay in sgrn/gateway/common/ until the memory/security
# interface-abstraction work lands.
target_link_libraries(sgrn_common INTERFACE
    sgrn::core
    sgrn::utils
    sgrn::scl
    fmt::fmt
    extern::rapidjson
    sgrn::s7codec
)

sgrn_install_component_library(
    TARGET sgrn_common
    EXPORT_NAME sgrn_common
    INCLUDE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/include/sgrn
    INCLUDE_DESTINATION include
)
