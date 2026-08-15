# cmake/deps/arm64.cmake — ARM64 cross-compilation dependency setup
# Included by cmake/deps.cmake when SGRN_CROSS_ARM64 is ON.

message(STATUS "[SGRN/arm64] Using ARM64 conda prefix: ${SGRN_CONDA_PREFIX}")

# Import repo-local prebuilt extern/ deps for the arm64 target
if(NOT TARGET extern::snap7cpp OR NOT TARGET extern::angelscript_addons)
    find_package(extern CONFIG QUIET PATHS "${SGRN_LOCAL_PREFIX}" NO_DEFAULT_PATH)
endif()

# Angelscript namespace
find_package(Angelscript CONFIG QUIET PATHS "${SGRN_LOCAL_PREFIX}" NO_DEFAULT_PATH)
if(TARGET Angelscript::angelscript AND NOT TARGET angelscript)
    add_library(angelscript INTERFACE IMPORTED GLOBAL)
    target_link_libraries(angelscript INTERFACE Angelscript::angelscript)
endif()

# open62541
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

# Standard system packages for the ARM64 target sysroot
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

find_path(ASIO_INCLUDE_DIR asio.hpp PATHS "${SGRN_CONDA_PREFIX}/include")
if(ASIO_INCLUDE_DIR)
    set(ASIO_FOUND TRUE)
    set(ASIO_INCLUDE_DIRS ${ASIO_INCLUDE_DIR})
    add_compile_definitions(ASIO_STANDALONE)
endif()
