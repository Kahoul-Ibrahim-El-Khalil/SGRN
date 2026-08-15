# cmake/deps/linux.cmake — Native Linux dependency discovery
# Included by cmake/deps.cmake when not cross-compiling.
# All sgrn::* INTERFACE alias targets are pre-declared by the root CMakeLists.

# Standard system/conda packages
find_package(ZLIB REQUIRED)
target_link_libraries(sgrn::zlib INTERFACE ZLIB::ZLIB)

find_package(zstd CONFIG REQUIRED)
target_link_libraries(sgrn::zstd INTERFACE zstd::libzstd)

find_package(OpenSSL REQUIRED)
target_link_libraries(sgrn::openssl INTERFACE OpenSSL::SSL OpenSSL::Crypto)

find_package(SQLite3 REQUIRED)
target_link_libraries(sgrn::sqlite INTERFACE SQLite::SQLite3)

find_package(cxxopts REQUIRED)
target_link_libraries(sgrn::cxxopts INTERFACE cxxopts::cxxopts)

if(NOT SGRN_SKIP_HOST_DEPS)
    if(NOT SGRN_USE_INSTALLED_DEPS)
        # In build-from-source mode: import repo-local extern/ built targets.
        # In installed-deps mode the root CMakeLists already ran find_package(extern)
        # which declares these targets from .prefix/<platform>/lib/cmake/extern/.
        if(NOT TARGET extern::snap7cpp OR NOT TARGET extern::angelscript_addons)
            find_package(extern CONFIG QUIET PATHS "${SGRN_LOCAL_PREFIX}" NO_DEFAULT_PATH)
        endif()

        # Ensure Angelscript namespace that extern:: targets depend on exists
        find_package(Angelscript CONFIG QUIET PATHS "${SGRN_LOCAL_PREFIX}" NO_DEFAULT_PATH)
        if(TARGET Angelscript::angelscript AND NOT TARGET angelscript)
            add_library(angelscript INTERFACE IMPORTED GLOBAL)
            target_link_libraries(angelscript INTERFACE Angelscript::angelscript)
        endif()

        # open62541 (OPC UA)
        if(NOT TARGET extern::open62541)
            find_package(open62541 CONFIG QUIET PATHS "${SGRN_LOCAL_PREFIX}" NO_DEFAULT_PATH)
            if(TARGET open62541::open62541)
                add_library(extern::open62541 INTERFACE IMPORTED GLOBAL)
                target_link_libraries(extern::open62541 INTERFACE open62541::open62541)
            elseif(TARGET open62541)
                add_library(extern::open62541 ALIAS open62541)
            endif()
        endif()

        include(${CMAKE_CURRENT_LIST_DIR}/fmt.cmake)
        include(${CMAKE_CURRENT_LIST_DIR}/jsoncpp_consume.cmake)
    else()
        # Installed-deps: extern-config.cmake declared fmt::fmt and JsonCpp::JsonCpp.
        # Wire sgrn::fmt and sgrn::jsoncpp from those.
        include(${CMAKE_CURRENT_LIST_DIR}/fmt.cmake)
        include(${CMAKE_CURRENT_LIST_DIR}/jsoncpp_consume.cmake)
    endif()


    # PostgreSQL
    if(NOT TARGET sgrn::postgres)
        add_library(sgrn::postgres INTERFACE)
    endif()
    find_package(PostgreSQL QUIET)
    if(PostgreSQL_FOUND)
        target_link_libraries(sgrn::postgres INTERFACE PostgreSQL::PostgreSQL)
    else()
        find_library(LIBPQ_PATH NAMES pq libpq PATHS "${LINUX_PREFIX}/lib")
        find_path(LIBPQ_INC  NAMES libpq-fe.h  PATHS "${LINUX_PREFIX}/include")
        if(LIBPQ_PATH AND LIBPQ_INC)
            target_link_libraries(sgrn::postgres INTERFACE "${LIBPQ_PATH}")
            target_include_directories(sgrn::postgres INTERFACE "${LIBPQ_INC}")
        endif()
    endif()

    # Redis / hiredis
    if(NOT TARGET sgrn::redis)
        add_library(sgrn::redis INTERFACE)
    endif()
    find_library(HIREDIS_PATH NAMES hiredis PATHS "${LINUX_PREFIX}/lib")
    if(HIREDIS_PATH)
        target_link_libraries(sgrn::redis INTERFACE "${HIREDIS_PATH}")
        target_include_directories(sgrn::redis INTERFACE "${LINUX_PREFIX}/include")
    endif()

    # Asio (Standalone)
    find_path(ASIO_INCLUDE_DIR asio.hpp PATHS "${LINUX_PREFIX}/include")
    if(ASIO_INCLUDE_DIR)
        set(ASIO_FOUND TRUE)
        set(ASIO_INCLUDE_DIRS ${ASIO_INCLUDE_DIR})
        add_compile_definitions(ASIO_STANDALONE)
    endif()
endif() # SGRN_SKIP_HOST_DEPS
