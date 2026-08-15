# cmake/deps/ixwebsocket.cmake
# Exposes: ixwebsocket::ixwebsocket

if(TARGET ixwebsocket::ixwebsocket)
    return()
endif()

sgrn_fetch_source(ixwebsocket)
set(USE_TLS ON CACHE BOOL "" FORCE)
add_subdirectory("${ixwebsocket_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/ixwebsocket"
    EXCLUDE_FROM_ALL
)
