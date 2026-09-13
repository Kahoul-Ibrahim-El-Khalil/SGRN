add_subdirectory(s7codec)

# OPC UA <-> S7 bridge codec table
# Depends on open62541 and s7codec; consumed by sgrn_gateway_opcua.
add_library(sgrn_opcua_codec_table STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/opcua_codec_table.cpp
)
add_library(sgrn::opcua_codec_table ALIAS sgrn_opcua_codec_table)

target_include_directories(sgrn_opcua_codec_table
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    # PRIVATE: only for the kTypeTable static_assert in opcua_codec_table.cpp
    # (DataType.hpp is header-only constexpr; no link dependency follows).
    PRIVATE ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
)

target_link_libraries(sgrn_opcua_codec_table
    PUBLIC  sgrn::s7codec
    PUBLIC  sgrn::open62541
)
