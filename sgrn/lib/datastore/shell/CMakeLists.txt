project(sgrn_datastore_shell LANGUAGES CXX C)

include(${CMAKE_SOURCE_DIR}/cmake/find_readline.cmake)
sgrn_find_readline()

add_library(sgrn_datastore_shell STATIC
    src/DatastoreShell.cpp
    include/sgrn/datastore/shell/DatastoreShell.hpp
)

target_include_directories(sgrn_datastore_shell
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(sgrn_datastore_shell
    PUBLIC
        sgrn_core
        sgrn_datastore_client
        sgrn_utils
        fmt::fmt
        ${SGRN_READLINE_LIBS}
)

add_library(sgrn::datastore_shell ALIAS sgrn_datastore_shell)
