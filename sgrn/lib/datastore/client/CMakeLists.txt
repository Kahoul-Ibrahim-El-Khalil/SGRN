project(sgrn_datastore_client LANGUAGES CXX C)

add_library(sgrn_datastore_client STATIC
    src/Client.cpp
    src/StorageClient.cpp
    src/client-json.cpp
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
        sgrn_datastore_wire
        fmt::fmt
        sgrn::httplib
)

add_library(sgrn::datastore_client ALIAS sgrn_datastore_client)
add_library(sgrn::sdk ALIAS sgrn_datastore_client)
add_library(sgrn_sdk ALIAS sgrn_datastore_client)

# Wire-decode micro-benchmark (not a functional test): measures the three
# client decode paths for the shared wire types (dynamic sgrn::json::Value
# round-trip vs direct JSON vs BEVE) over 1000 ActiveSession records.
if(SGRN_BUILD_TESTS)
    add_executable(wire_decode_benchmark test/wire-decode-benchmark.cpp)
    target_link_libraries(wire_decode_benchmark PRIVATE sgrn_datastore_wire fmt::fmt)
    add_test(NAME wire_decode_benchmark COMMAND wire_decode_benchmark)
endif()
