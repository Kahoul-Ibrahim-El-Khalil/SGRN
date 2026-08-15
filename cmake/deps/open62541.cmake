# cmake/deps/open62541.cmake
# Exposes: extern::open62541

if(TARGET open62541 OR TARGET extern::open62541)
    return()
endif()

sgrn_fetch_source(open62541)

set(UA_BUILD_UNIT_TESTS  OFF CACHE BOOL "" FORCE)
set(UA_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(UA_BUILD_TOOLS       OFF CACHE BOOL "" FORCE)
set(UA_ENABLE_AMALGAMATION OFF CACHE BOOL "" FORCE)

# Skip open62541's `check_ipo_supported()` LTO check (CMakeLists.txt:656).
# It tries to compile+run a try_compile program and fails under CMake 4.2 in
# this clang→MinGW cross-compilation ("Fail to copy destinationfile").
# Defining CMAKE_INTERPROCEDURAL_OPTIMIZATION (even as OFF) makes the
# `NOT DEFINED` guard false so the broken check is never executed.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL
    "Disable open62541 IPO detection (breaks under CMake 4.x cross-compilation)" FORCE)

add_subdirectory("${open62541_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/open62541"
    EXCLUDE_FROM_ALL
)

if(TARGET open62541)
    add_library(extern::open62541 ALIAS open62541)
endif()
