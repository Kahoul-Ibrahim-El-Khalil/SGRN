# ── OPENER (ETHERNET/IP) ──────────────────────────────────────────────────────
# [ignoring loop detection]
# Compiles the OpENer EtherNet/IP Stack as a static library.
# Source is obtained via FetchContent (declared in FetchDeps.cmake).
# Exposes the target:
#   extern::opener

if(TARGET opener)
    return()
endif()

sgrn_fetch_source(opener)

if(EXISTS "${opener_SOURCE_DIR}/source/src")
    set(OPENER_SRC_DIR "${opener_SOURCE_DIR}/source/src")
else()
    set(OPENER_SRC_DIR "${opener_SOURCE_DIR}/src")
endif()

set(OPENER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/opener_gen)
file(MAKE_DIRECTORY ${OPENER_GEN_DIR})

# Configure devicedata.h
set(OpENer_Device_Config_Vendor_Id 1)
set(OpENer_Device_Config_Device_Type 12)
set(OpENer_Device_Config_Product_Code 65001)
set(OpENer_Device_Config_Device_Name "SGRN EtherNet/IP Adapter")
set(OpENer_Device_Major_Version 2)
set(OpENer_Device_Minor_Version 3)
set(OpENer_VERSION_MAJOR 2)
set(OpENer_VERSION_MINOR 3)

configure_file(
    "${OPENER_SRC_DIR}/ports/devicedata.h.in"
    "${OPENER_GEN_DIR}/devicedata.h"
)

# Write opener_user_conf.h
file(WRITE "${OPENER_GEN_DIR}/opener_user_conf.h" [=[
#ifndef OPENER_USER_CONF_H_
#define OPENER_USER_CONF_H_

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef unsigned short in_port_t;
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <sys/select.h>
#endif

#include <assert.h>
#include "typedefs.h"
#include "devicedata.h"

#ifndef CIP_FILE_OBJECT
  #define CIP_FILE_OBJECT 0
#endif

#ifndef CIP_SECURITY_OBJECTS
  #define CIP_SECURITY_OBJECTS 0
#endif

#ifndef OPENER_IS_DLR_DEVICE
  #define OPENER_IS_DLR_DEVICE  0
#endif

#ifndef OPENER_TCPIP_IFACE_CFG_SETTABLE
  #define OPENER_TCPIP_IFACE_CFG_SETTABLE 0
#endif

#ifndef OPENER_ETHLINK_INSTANCE_CNT
  #define OPENER_ETHLINK_INSTANCE_CNT  1
#endif

#ifndef OPENER_ETHLINK_LABEL_ENABLE
  #define OPENER_ETHLINK_LABEL_ENABLE  0
#endif

#ifndef OPENER_ETHLINK_CNTRS_ENABLE
  #define OPENER_ETHLINK_CNTRS_ENABLE 0
#endif

#ifndef OPENER_ETHLINK_IFACE_CTRL_ENABLE
  #define OPENER_ETHLINK_IFACE_CTRL_ENABLE 0
#endif

#ifndef OPENER_MESSAGE_DATA_REPLY_BUFFER
  #define OPENER_MESSAGE_DATA_REPLY_BUFFER 500
#endif

#define OPENER_CIP_NUM_APPLICATION_SPECIFIC_CONNECTABLE_OBJECTS 1
#define OPENER_CIP_NUM_EXPLICIT_CONNS 6
#define OPENER_CIP_NUM_EXLUSIVE_OWNER_CONNS 1
#define OPENER_CIP_NUM_INPUT_ONLY_CONNS 1
#define OPENER_CIP_NUM_INPUT_ONLY_CONNS_PER_CON_PATH 3
#define OPENER_CIP_NUM_LISTEN_ONLY_CONNS 1
#define OPENER_CIP_NUM_LISTEN_ONLY_CONNS_PER_CON_PATH   3
#define OPENER_NUMBER_OF_SUPPORTED_SESSIONS 20

static const MilliSeconds kOpenerTimerTickInMilliSeconds = 10;

#ifndef OPENER_UNIT_TEST
  #define OPENER_ASSERT(assertion) assert(assertion);
#endif

#endif /* OPENER_USER_CONF_H_ */
]=])

# Case-sensitivity wrapper for Windows cross-compilation on case-sensitive filesystems
file(WRITE "${OPENER_GEN_DIR}/Ws2tcpip.h" "#include <ws2tcpip.h>\n")


# Platform port selection
if(WIN32)
    set(OPENER_PORT_DIR ${OPENER_SRC_DIR}/ports/WIN32)
else()
    set(OPENER_PORT_DIR ${OPENER_SRC_DIR}/ports/POSIX)
endif()

# Dynamically gather all source files
file(GLOB OPENER_CIP_SOURCES "${OPENER_SRC_DIR}/cip/*.c")
file(GLOB OPENER_ENCAP_SOURCES "${OPENER_SRC_DIR}/enet_encap/*.c")
file(GLOB OPENER_UTILS_SOURCES "${OPENER_SRC_DIR}/utils/*.c")
file(GLOB OPENER_PORT_GENERIC_SOURCES "${OPENER_SRC_DIR}/ports/*.c")
file(GLOB OPENER_PORT_SOURCES "${OPENER_PORT_DIR}/*.c")

# Exclude sample application main entrypoint
list(REMOVE_ITEM OPENER_PORT_SOURCES "${OPENER_PORT_DIR}/main.c")

set(OPENER_SOURCES
    ${OPENER_CIP_SOURCES}
    ${OPENER_ENCAP_SOURCES}
    ${OPENER_UTILS_SOURCES}
    ${OPENER_PORT_GENERIC_SOURCES}
    ${OPENER_PORT_SOURCES}
)

add_library(opener STATIC ${OPENER_SOURCES})
add_library(extern::opener ALIAS opener)

target_include_directories(opener SYSTEM PUBLIC
    $<BUILD_INTERFACE:${OPENER_SRC_DIR}>
    $<BUILD_INTERFACE:${OPENER_SRC_DIR}/cip>
    $<BUILD_INTERFACE:${OPENER_SRC_DIR}/enet_encap>
    $<BUILD_INTERFACE:${OPENER_SRC_DIR}/utils>
    $<BUILD_INTERFACE:${OPENER_SRC_DIR}/ports>
    $<BUILD_INTERFACE:${OPENER_PORT_DIR}>
    $<BUILD_INTERFACE:${OPENER_GEN_DIR}>
)

set_target_properties(opener PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

target_compile_definitions(opener PUBLIC
    PC_OPENER_ETHERNET_BUFFER_SIZE=4096
    $<$<COMPILE_LANGUAGE:CXX>:RESTRICT=>
)

if(WIN32)
    target_link_libraries(opener PUBLIC ws2_32 iphlpapi)
    target_compile_definitions(opener PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
    target_compile_definitions(opener PUBLIC
        $<$<COMPILE_LANGUAGE:C>:RESTRICT=__restrict>
    )
else()
    target_link_libraries(opener PUBLIC pthread)
    target_compile_options(opener PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-include> $<$<COMPILE_LANGUAGE:C>:net/if.h>
    )
    target_compile_definitions(opener PRIVATE
        _GNU_SOURCE
        _DEFAULT_SOURCE
        _POSIX_C_SOURCE=200112L
        OPENER_POSIX
    )
    target_compile_definitions(opener PUBLIC
        $<$<COMPILE_LANGUAGE:C>:RESTRICT=restrict>
    )
endif()

sgrn_add_to_install(opener)
