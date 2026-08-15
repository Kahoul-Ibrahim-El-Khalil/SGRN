# cmake/deps/libmodbus.cmake
# Exposes: extern::modbus (static, C linkage)
# config.h and modbus-version.h are generated into the binary dir (no autotools needed).

if(TARGET modbus)
    return()
endif()

sgrn_fetch_source(libmodbus)
set(LIBMODBUS_DIR ${libmodbus_SOURCE_DIR}/src)
set(LIBMODBUS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/libmodbus_gen")
file(MAKE_DIRECTORY "${LIBMODBUS_GEN_DIR}")

file(WRITE "${LIBMODBUS_GEN_DIR}/config.h" [=[
#ifndef CONFIG_H
#define CONFIG_H
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#ifndef _WIN32
#  define HAVE_SYS_SOCKET_H 1
#  define HAVE_ARPA_INET_H 1
#  define HAVE_NETINET_IN_H 1
#  define HAVE_NETINET_IP_H 1
#  define HAVE_NETINET_TCP_H 1
#  define HAVE_UNISTD_H 1
#endif
#define PACKAGE_VERSION "3.2.0"
#endif
]=])

file(WRITE "${LIBMODBUS_GEN_DIR}/modbus-version.h" [=[
#ifndef MODBUS_VERSION_H
#define MODBUS_VERSION_H
#define LIBMODBUS_VERSION_MAJOR 3
#define LIBMODBUS_VERSION_MINOR 2
#define LIBMODBUS_VERSION_MICRO 0
#define LIBMODBUS_VERSION_STRING "3.2.0"
#endif
]=])

add_library(modbus STATIC
    ${LIBMODBUS_DIR}/modbus.c
    ${LIBMODBUS_DIR}/modbus-data.c
    ${LIBMODBUS_DIR}/modbus-rtu.c
    ${LIBMODBUS_DIR}/modbus-tcp.c
)
add_library(extern::modbus ALIAS modbus)
target_include_directories(modbus SYSTEM PUBLIC
    $<BUILD_INTERFACE:${LIBMODBUS_DIR}>
    $<BUILD_INTERFACE:${LIBMODBUS_GEN_DIR}>
    $<INSTALL_INTERFACE:include/modbus>
)
set_target_properties(modbus PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11 C_STANDARD_REQUIRED ON)

if(WIN32)
    target_link_libraries(modbus PUBLIC ws2_32)
    target_compile_definitions(modbus PRIVATE OS_WIN32 _CRT_SECURE_NO_WARNINGS)
    # On Windows, setsockopt() takes const char* for optval (not const void* like POSIX).
    # libmodbus passes int* in several places; suppress the incompatible-pointer-types
    # errors that arise from this POSIX-vs-Windows API difference.
    target_compile_options(modbus PRIVATE -Wno-incompatible-pointer-types)
else()
    target_compile_definitions(modbus PRIVATE HAVE_INTTYPES_H=1 HAVE_STDINT_H=1 HAVE_NETINET_IN_H=1 HAVE_NETINET_IP_H=1)
endif()

sgrn_add_to_install(modbus)
