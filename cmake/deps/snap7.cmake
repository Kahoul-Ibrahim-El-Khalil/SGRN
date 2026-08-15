# cmake/deps/snap7.cmake
# Exposes: extern::snap7cpp (static, all platforms), extern::snap7cpp_shared (DLL, Windows)

sgrn_fetch_source(snap7)
set(SNAP7_DIR ${snap7_SOURCE_DIR})
set(SNAP7_WRAP ${SNAP7_DIR}/release/wrappers/c-cpp)
set(SNAP7_SOURCES
    ${SNAP7_DIR}/src/sys/snap_msgsock.cpp
    ${SNAP7_DIR}/src/sys/snap_sysutils.cpp
    ${SNAP7_DIR}/src/sys/snap_tcpsrvr.cpp
    ${SNAP7_DIR}/src/sys/snap_threads.cpp
    ${SNAP7_DIR}/src/core/s7_client.cpp
    ${SNAP7_DIR}/src/core/s7_isotcp.cpp
    ${SNAP7_DIR}/src/core/s7_partner.cpp
    ${SNAP7_DIR}/src/core/s7_peer.cpp
    ${SNAP7_DIR}/src/core/s7_server.cpp
    ${SNAP7_DIR}/src/core/s7_text.cpp
    ${SNAP7_DIR}/src/core/s7_micro_client.cpp
    ${SNAP7_DIR}/src/lib/snap7_libmain.cpp
    ${SNAP7_WRAP}/snap7.cpp
)
set(SNAP7_INCS
    $<BUILD_INTERFACE:${SNAP7_DIR}/src/sys>
    $<BUILD_INTERFACE:${SNAP7_DIR}/src/core>
    $<BUILD_INTERFACE:${SNAP7_DIR}/src/lib>
    $<BUILD_INTERFACE:${SNAP7_WRAP}>
    $<INSTALL_INTERFACE:include/snap7cpp>
)

if(WIN32 AND NOT TARGET snap7cpp_shared)
    add_library(snap7cpp_shared SHARED ${SNAP7_SOURCES})
    target_include_directories(snap7cpp_shared PUBLIC ${SNAP7_INCS})
    target_link_libraries(snap7cpp_shared PUBLIC ws2_32 winmm)
    set_target_properties(snap7cpp_shared PROPERTIES OUTPUT_NAME "snap7cpp" CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_library(extern::snap7cpp_shared ALIAS snap7cpp_shared)
    sgrn_add_to_install(snap7cpp_shared)
endif()

if(NOT TARGET snap7cpp)
    add_library(snap7cpp STATIC ${SNAP7_SOURCES})
    target_include_directories(snap7cpp PUBLIC ${SNAP7_INCS})
    set_target_properties(snap7cpp PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    if(WIN32)
        target_link_libraries(snap7cpp PUBLIC ws2_32 winmm)
        target_link_options(snap7cpp PRIVATE "-fuse-ld=lld")
    else()
        target_link_libraries(snap7cpp PUBLIC pthread rt)
    endif()
    add_library(extern::snap7cpp ALIAS snap7cpp)
    sgrn_add_to_install(snap7cpp)
endif()
