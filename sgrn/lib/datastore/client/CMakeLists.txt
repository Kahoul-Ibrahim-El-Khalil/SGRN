project(sgrn_datastore_client LANGUAGES CXX C)

add_library(sgrn_datastore_client STATIC
    src/Client.cpp
    src/StorageClient.cpp
    include/sgrn/datastore/client/Client.hpp
    include/sgrn/datastore/client/StorageClient.hpp
    include/sgrn/datastore/client/Types.hpp
)

target_include_directories(sgrn_datastore_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(sgrn_datastore_client
    PUBLIC
        sgrn_core
        sgrn_utils
        fmt::fmt
        sgrn::httplib
        sgrn::rapidjson
)

add_library(sgrn::datastore_client ALIAS sgrn_datastore_client)
add_library(sgrn::sdk ALIAS sgrn_datastore_client)
add_library(sgrn_sdk ALIAS sgrn_datastore_client)
