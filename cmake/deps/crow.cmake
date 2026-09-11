# cmake/deps/crow.cmake
# Exposes: crow, extern::crow (header-only INTERFACE targets)
#
# Crow is the asio-based HTTP + WebSocket server backing the gateway
# northbound adapters (replaces cpp-httplib + IXWebSocket there). It is
# header-only and uses standalone asio — the same asio the gateway already
# runs on — so HTTP and WebSocket share one event loop and one listener.
#
# NOTE: this file intentionally fetches via CPM even in SGRN_USE_INSTALLED_DEPS
# mode when the staged prefix does not provide crow yet (the prefix is only
# rebuilt on demand via the deps presets).

if(TARGET crow OR TARGET extern::crow)
    return()
endif()

sgrn_fetch_source(crow)

add_library(crow INTERFACE)
target_include_directories(crow SYSTEM INTERFACE "${crow_SOURCE_DIR}/include")
# Standalone asio (never Boost): matches the gateway's ASIO_STANDALONE usage.
target_compile_definitions(crow INTERFACE ASIO_STANDALONE)
if(ASIO_FOUND AND ASIO_INCLUDE_DIRS)
    target_include_directories(crow SYSTEM INTERFACE ${ASIO_INCLUDE_DIRS})
endif()
target_link_libraries(crow INTERFACE Threads::Threads)
add_library(extern::crow ALIAS crow)
