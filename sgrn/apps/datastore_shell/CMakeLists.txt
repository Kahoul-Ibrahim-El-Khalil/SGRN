# datastore_shell — unix-like shell & AngelScript REPL for the datastore API (see README.md).
include(${CMAKE_SOURCE_DIR}/cmake/find_readline.cmake)
sgrn_find_readline()

sgrn_add_component_executable(
    TARGET      datastore_shell
    SOURCES     main.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_datastore_shell
)
target_include_directories(datastore_shell PRIVATE
    ${CMAKE_SOURCE_DIR}/sgrn/lib/datastore/shell/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/datastore/client/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/utils/include
    ${CMAKE_SOURCE_DIR}/sgrn/lib/core/include
)
target_link_libraries(datastore_shell PRIVATE ${SGRN_READLINE_LIBS})
sgrn_target(datastore_shell)
sgrn_install_component_executable(TARGET datastore_shell COMPONENT backend)
