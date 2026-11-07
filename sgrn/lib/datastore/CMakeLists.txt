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
    add_executable(crud_binds_test tests/crud-binds-test.cpp)
    target_link_libraries(crud_binds_test PRIVATE sgrn_datastore_lib)
    add_test(NAME crud_binds_test COMMAND crud_binds_test)
    # Rate limiter pure-logic tests (classification, config, key building):
    # no external services, no app singleton, no live database required.
    add_executable(rate_limit_test tests/rate-limit-test.cpp)
    target_link_libraries(rate_limit_test PRIVATE sgrn_datastore_lib)
    add_test(NAME rate_limit_test COMMAND rate_limit_test)
    # System-config helper tests (schema/sanitize/validate+merge): pure Json
    # logic, no Drogon app singleton, no filesystem writes.
    add_executable(system_config_test tests/system-config-test.cpp)
    target_link_libraries(system_config_test PRIVATE sgrn_datastore_lib)
    add_test(NAME system_config_test COMMAND system_config_test)
    # Live-PostgreSQL auth + SQL envelope regression tests (postgres/functions
    # authenticate_*; storage.cpp jsonb envelopes). Uses the shared
    # sgrn::test fixtures (external PG* env or ephemeral initdb/postgres
    # cluster); SKIPs with exit 0 when no PostgreSQL is available.
    add_executable(pg_auth_functions_test tests/pg-auth-functions-test.cpp)
    target_link_libraries(pg_auth_functions_test PRIVATE sgrn_datastore_lib reproc::reproc++)
    target_include_directories(pg_auth_functions_test PRIVATE ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support)
    add_test(NAME pg_auth_functions_test COMMAND pg_auth_functions_test)
    # Backup & ustar archive tests (tar writing, SHA-256 streaming, directory hygiene)
    add_executable(backup_test tests/backup-test.cpp)
    target_link_libraries(backup_test PRIVATE sgrn_datastore_lib)
    add_test(NAME backup_test COMMAND backup_test)
    # Verification & hardening tests (verify command catches planted misconfigurations)
    add_executable(verify_test tests/verify-test.cpp)
    target_link_libraries(verify_test PRIVATE sgrn_datastore_lib OpenSSL::Crypto)
    add_test(NAME verify_test COMMAND verify_test)
    # ASan scratch test (out-of-bounds read detection under sanitizers)
    add_executable(asan_scratch_test tests/asan-scratch-test.cpp)
    target_link_libraries(asan_scratch_test PRIVATE sgrn_datastore_lib)
    add_test(NAME asan_scratch_test COMMAND asan_scratch_test)
    # Cross-organisation authorization & audit purge regression tests
    add_executable(cross_org_auth_test tests/cross-org-auth-test.cpp)
    target_link_libraries(cross_org_auth_test PRIVATE sgrn_datastore_lib)
    add_test(NAME cross_org_auth_test COMMAND cross_org_auth_test)
    # Scoped admins and domain hierarchy tests (Prompt 4)
    add_executable(scoped_admins_test tests/scoped-admins-test.cpp)
    target_link_libraries(scoped_admins_test PRIVATE sgrn_datastore_lib reproc::reproc++)
    target_include_directories(scoped_admins_test PRIVATE ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support)
    target_compile_definitions(scoped_admins_test PRIVATE SGRN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME scoped_admins_test COMMAND scoped_admins_test)
    # Authorization matrix and privilege escalation tests (Prompt 5)
    add_executable(authz_matrix_test tests/authz-matrix-test.cpp)
    target_link_libraries(authz_matrix_test PRIVATE sgrn_datastore_lib reproc::reproc++)
    target_include_directories(authz_matrix_test PRIVATE ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support)
    target_compile_definitions(authz_matrix_test PRIVATE SGRN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME authz_matrix_test COMMAND authz_matrix_test)
    # Tamper-evident audit log tests (Prompt 6)
    add_executable(audit_tamper_test tests/audit-tamper-test.cpp)
    target_link_libraries(audit_tamper_test PRIVATE sgrn_datastore_lib reproc::reproc++ OpenSSL::Crypto)
    target_include_directories(audit_tamper_test PRIVATE ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support)
    target_compile_definitions(audit_tamper_test PRIVATE SGRN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME audit_tamper_test COMMAND audit_tamper_test)
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
