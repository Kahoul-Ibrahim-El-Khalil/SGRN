# sgrn_datastore — backend server executable. Entry point lives here; asset
# generation (dashboard/SQL/config headers) stays in sgrn/lib/datastore/src and
# shares its GEN_DIRs via cache variables (see sgrn/lib/datastore/src/CMakeLists.txt).
add_executable(sgrn_datastore
    main.cpp
    "${DASHBOARD_GEN_DIR}/web_assets.hpp"
    "${SQL_GEN_DIR}/sql_assets.hpp"
    "${CONFIG_GEN_DIR}/config_assets.hpp"
)
sgrn_use_pch(sgrn_datastore sgrn_pch_net)
add_dependencies(sgrn_datastore sgrn_dashboard_assets sgrn_sql_assets sgrn_config_assets)
target_include_directories(sgrn_datastore PRIVATE "${DASHBOARD_GEN_DIR}" "${SQL_GEN_DIR}" "${CONFIG_GEN_DIR}")

# Link against the shared backend and set RPATH
# Static archives that rely on Drogon's global-constructor route registration
# (routes() static members) must be forced-included in their entirety, because
# --as-needed causes the linker to drop archive members whose undefined
# references appear only in the .o that comes *before* the .a in the command.
if(SGRN_BUILD_STATIC)
    target_link_libraries(sgrn_datastore PUBLIC
        -Wl,--whole-archive
        sgrn_datastore_lib
        -Wl,--no-whole-archive
    )
else()
    target_link_libraries(sgrn_datastore PUBLIC sgrn_datastore_lib)
endif()
set_target_properties(sgrn_datastore PROPERTIES INSTALL_RPATH "$ORIGIN")
