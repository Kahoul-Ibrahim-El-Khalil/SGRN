# wire — shared wire types (auth, storage, admin, common) with Glaze reflection.
# Linked by both the datastore server (sgrn_datastore_lib) and the clients
# (sgrn_datastore_client, sgrn_datastore_shell) so both ends speak the same
# concrete types over JSON, MessagePack, or BEVE.
#
# The headers hold only the type definitions (required wherever sgrn::Result<T>,
# readBeve<T>() or `T body;` is instantiated); wire.cpp holds the compile-time
# checks. Never forward-declare these types in the headers.

add_library(sgrn_datastore_wire STATIC
    src/wire.cpp
)

target_include_directories(sgrn_datastore_wire PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sgrn_datastore_wire PUBLIC
    sgrn_utils
)
