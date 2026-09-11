# SGRN applications — one directory per entry point (main.cpp + CMakeLists.txt).
#
# Library targets live in the sibling component dirs (lib code); apps/ only
# wires mains to libs. Each subdir is gated on the same SGRN_BUILD_* option
# as the library it needs, mirroring sgrn/CMakeLists.txt.

find_package(Threads REQUIRED)

# Shared Windows system libs for the gateway/s7shell-family apps below.
set(WIN_LIBS_LIST
    ws2_32 winmm rpcrt4 crypt32 advapi32 iphlpapi
    secur32 wldap32 bcrypt userenv
)

if(SGRN_BUILD_GATEWAY)
    add_subdirectory(gateway)
    add_subdirectory(mbproxy)
    add_subdirectory(wal_dump)
    add_subdirectory(dataset)
    add_subdirectory(replay)
    # sclc belongs to the scl component, which is SGRN_BUILD_GATEWAY-gated.
    add_subdirectory(sclc)
endif()

if(SGRN_BUILD_S7)
    add_subdirectory(s7shell)
endif()

if(SGRN_BUILD_DATASTORE)
    add_subdirectory(datastore)
endif()

# =============================================================================
# Runtime packaging (moved here from the component files: targets must exist
# before these calls run, and apps/ configures after all libraries).
# =============================================================================

# Gateway component bundle.
sgrn_package_runtime_dependencies(
    APPS
        gateway
        mbproxy
        sgrn_dataset
        sgrn_replay
    LIBS
        sgrn_gateway
    COMPONENT gateway
)

# SCL component bundle.
sgrn_package_runtime_dependencies(
    APPS
        sclc
    LIBS
        sgrn_scl
        sgrn_utils
    COMPONENT sclc
)

# S7Shell component bundle.
sgrn_package_runtime_dependencies(
    APPS
        s7shell
    LIBS
        sgrn::wrappers::s7
        sgrn_gateway_s7
        sgrn_gateway_security
        sgrn_gateway_twin
        sgrn_plcsim
        sgrn_s7shell
        sgrn_scl
        sgrn_utils
    EXTERNAL_DEPS
        sgrn::snap7
        sgrn::fmt
        ixwebsocket::ixwebsocket
    COMPONENT s7shell
)

# Backend component bundle.
sgrn_package_runtime_dependencies(
    APPS
        sgrn_datastore
    LIBS
        sgrn_datastore_lib
        sgrn_datastore_orm
        sgrn_utils
        sgrn_sdk
    EXTERNAL_DEPS
        sgrn::drogon
        sgrn::trantor
        sgrn::fmt
        sgrn::jsoncpp
        sgrn::openssl
        sgrn::postgres
        sgrn::zstd
        sgrn::redis
        sgrn::aws
    COMPONENT backend
)
