project(SGRN_BACKEND LANGUAGES C CXX)

# ==================== Build Options ====================
option(RELEASE "Enable aggressive release optimizations" OFF)
option(SANITIZE "Enable sanitizers" OFF)
option(BUILD_ORM "Build SGRN ORM" ON)
if(NOT DEFINED BUILD_TESTS)
    set(BUILD_TESTS OFF CACHE BOOL "Build tests")
endif()
if(BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/tests")
    add_subdirectory(src/tests)
endif()

# ==================== Shared Extern Modules ====================
set(DROGON_LIBRARIES sgrn::drogon)

# ==================== SGRN Core Interface ====================
include(cmake/sgrn_datastore_core.cmake)

# ==================== Project Subdirectories ====================
add_subdirectory(src)

# ==================== Tests ====================
# CRUD view bind tests: pure validation (helper) plus HTTP-level 400 checks
# that run before any DB access, so no live database is required.
if(SGRN_BUILD_TESTS)
    add_executable(crud_binds_test tests/crud_binds_test.cpp)
    target_link_libraries(crud_binds_test PRIVATE sgrn_datastore_lib)
    add_test(NAME crud_binds_test COMMAND crud_binds_test)
endif()

# ==================== Summary ====================
message(STATUS "==========================================")
message(STATUS "SGRN Build Configuration Summary")
message(STATUS "==========================================")
message(STATUS "BUILD_ORM: ${BUILD_ORM}")
message(STATUS "BUILD_TESTS: ${BUILD_TESTS}")
message(STATUS "SANITIZE: ${SANITIZE}")
message(STATUS "RELEASE: ${RELEASE}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "==========================================")
