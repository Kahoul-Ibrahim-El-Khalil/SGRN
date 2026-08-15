# ==============================================================================
# SGRN Backend Unified Consolidation
# ==============================================================================

set(BACKEND_SOURCES "")
set(ORM_SOURCES "")

# 1. Plugins discovery
file(GLOB_RECURSE PLUGIN_SOURCES plugins/*.cpp)
list(APPEND BACKEND_SOURCES ${PLUGIN_SOURCES})

# 2. ORM MODELS (Independent Shared Library)
if(BUILD_ORM)
    file(GLOB_RECURSE ORM_SOURCES orm/models/*.cc)
    # Apply warning suppression to ORM files
    foreach(f ${ORM_SOURCES})
        set_source_files_properties(${f} PROPERTIES COMPILE_FLAGS "-w")
    endforeach()
    
    if(SGRN_BUILD_STATIC)
        add_library(sgrn_datastore_orm STATIC ${ORM_SOURCES})
    else()
        add_library(sgrn_datastore_orm SHARED ${ORM_SOURCES})
    endif()
    set_target_properties(sgrn_datastore_orm PROPERTIES 
        CXX_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
        INSTALL_RPATH "$ORIGIN"
    )
    target_link_libraries(sgrn_datastore_orm PUBLIC sgrn_datastore_core sgrn::drogon)

    # Some prebuilt Drogon configurations may compile with brotli enabled but
    # not propagate brotli link libs (static libs). Ensure runtime is link-complete.
    if(UNIX AND NOT APPLE)
        find_library(SGRN_BROTLIDEC_LIBRARY NAMES brotlidec PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
        find_library(SGRN_BROTLIENC_LIBRARY NAMES brotlienc PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
        find_library(SGRN_BROTLICOMMON_LIBRARY NAMES brotlicommon PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
        if(SGRN_BROTLIDEC_LIBRARY AND SGRN_BROTLIENC_LIBRARY AND SGRN_BROTLICOMMON_LIBRARY)
            # IMPORTANT: the project-wide toolchain injects `-Wl,--as-needed`,
            # which can drop these libs (they are only referenced by objects
            # pulled from static libdrogon.a). Force the NEEDED entries so the
            # symbols resolve at runtime.
            target_link_options(sgrn_datastore_orm PRIVATE "LINKER:--no-as-needed")
            target_link_libraries(sgrn_datastore_orm PRIVATE
                "${SGRN_BROTLIDEC_LIBRARY}"
                "${SGRN_BROTLIENC_LIBRARY}"
                "${SGRN_BROTLICOMMON_LIBRARY}"
            )
        endif()

        # libuuid is provided by the SGRN toolset and is required by some
        # transitive dependencies (e.g. Trantor/Drogon on some builds).
        find_library(SGRN_UUID_LIBRARY NAMES uuid PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
        if(SGRN_UUID_LIBRARY)
            target_link_libraries(sgrn_datastore_orm PRIVATE "${SGRN_UUID_LIBRARY}")
        endif()

        find_library(SGRN_SQLITE3_LIBRARY NAMES sqlite3 PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
        if(SGRN_SQLITE3_LIBRARY)
            target_link_libraries(sgrn_datastore_orm PRIVATE "${SGRN_SQLITE3_LIBRARY}")
        endif()
    endif()
endif()

# 3. SERVICES
file(GLOB_RECURSE SERVICES_SOURCES services/*.cpp)
list(APPEND BACKEND_SOURCES ${SERVICES_SOURCES})

# 4. FILTERS
file(GLOB_RECURSE FILTERS_SOURCES filters/*.cpp)
list(APPEND BACKEND_SOURCES ${FILTERS_SOURCES})

# 5. HANDLERS
file(GLOB_RECURSE HANDLERS_SOURCES handlers/*.cpp)
list(APPEND BACKEND_SOURCES ${HANDLERS_SOURCES})

# ── The Fat Backend Library ──────────────────────────────────────────────────
if(SGRN_BUILD_STATIC)
    add_library(sgrn_datastore_lib STATIC ${BACKEND_SOURCES})
else()
    add_library(sgrn_datastore_lib SHARED ${BACKEND_SOURCES})
endif()

# Visibility and RPATH only matter for shared builds
if(NOT SGRN_BUILD_STATIC)
    set_target_properties(sgrn_datastore_lib PROPERTIES 
        CXX_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
        INSTALL_RPATH "$ORIGIN"
    )
endif()

target_include_directories(sgrn_datastore_lib PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
)

# Unified linkage for the whole backend
target_link_libraries(sgrn_datastore_lib PUBLIC
    sgrn_datastore_core
    sgrn_datastore_orm
    sgrn::drogon
    sgrn::trantor
    sgrn::postgres
    sgrn::jsoncpp
    sgrn::fmt
    sgrn::openssl
    sgrn::zstd
    sgrn::redis
    sgrn::aws
    sgrn::utils
    sgrn::core
)

if(UNIX AND NOT APPLE)
    find_library(SGRN_SQLITE3_LIBRARY NAMES sqlite3 PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
    if(SGRN_SQLITE3_LIBRARY)
        target_link_options(sgrn_datastore_lib PRIVATE "LINKER:--no-as-needed")
        target_link_libraries(sgrn_datastore_lib PRIVATE "${SGRN_SQLITE3_LIBRARY}")
    endif()

    find_library(SGRN_CARES_LIBRARY NAMES cares c-ares PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
    if(SGRN_CARES_LIBRARY)
        target_link_libraries(sgrn_datastore_lib PRIVATE "${SGRN_CARES_LIBRARY}")
    endif()
    
    find_library(SGRN_UUID_LIBRARY NAMES uuid PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
    if(SGRN_UUID_LIBRARY)
        target_link_libraries(sgrn_datastore_lib PRIVATE "${SGRN_UUID_LIBRARY}")
    endif()

    find_library(SGRN_BROTLIDEC_LIBRARY NAMES brotlidec PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
    find_library(SGRN_BROTLIENC_LIBRARY NAMES brotlienc PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
    find_library(SGRN_BROTLICOMMON_LIBRARY NAMES brotlicommon PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
    if(SGRN_BROTLIDEC_LIBRARY AND SGRN_BROTLIENC_LIBRARY AND SGRN_BROTLICOMMON_LIBRARY)
        target_link_libraries(sgrn_datastore_lib PRIVATE "${SGRN_BROTLIDEC_LIBRARY}" "${SGRN_BROTLIENC_LIBRARY}" "${SGRN_BROTLICOMMON_LIBRARY}")
    endif()
endif()

# ── Compatibility Aliases ────────────────────────────────────────────────────
add_library(sgrn_datastore_plugins ALIAS sgrn_datastore_lib)
add_library(sgrn_datastore_services ALIAS sgrn_datastore_lib)
add_library(sgrn_datastore_handlers ALIAS sgrn_datastore_lib)
add_library(sgrn_datastore_filters ALIAS sgrn_datastore_lib)

# ── Dashboard Web Assets ─────────────────────────────────────────────────────
set(DASHBOARD_SRC_DIR "${PROJECT_SOURCE_DIR}/web/dashboard")
set(DASHBOARD_BUILD_DIR "${DASHBOARD_SRC_DIR}/build")
set(DASHBOARD_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/dashboard_generated")

# Stamp file to track when bun install was last run
set(DASHBOARD_INSTALL_STAMP "${CMAKE_CURRENT_BINARY_DIR}/bun_install.stamp")

# Step 1a: install dependencies (only when package.json or bun.lock changes)
add_custom_command(
    OUTPUT "${DASHBOARD_INSTALL_STAMP}"
    COMMAND bun install
    COMMAND ${CMAKE_COMMAND} -E touch "${DASHBOARD_INSTALL_STAMP}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/web"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/web/package.json"
        "${PROJECT_SOURCE_DIR}/web/bun.lock"
    COMMENT "[Dashboard] Installing JS dependencies via bun..."
    VERBATIM
)

# Step 1b: bun vite build → produces dashboard/build/
add_custom_command(
    OUTPUT "${DASHBOARD_BUILD_DIR}/index.html"
    COMMAND bun run build
    WORKING_DIRECTORY "${DASHBOARD_SRC_DIR}"
    DEPENDS
        "${DASHBOARD_INSTALL_STAMP}"
        "${DASHBOARD_SRC_DIR}/vite.config.ts"
        "${DASHBOARD_SRC_DIR}/package.json"
    COMMENT "[Dashboard] Building React app via bun vite build..."
    VERBATIM
)

# Step 2: compress all build output → C++ headers
add_custom_command(
    OUTPUT "${DASHBOARD_GEN_DIR}/web_assets.hpp"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DASHBOARD_GEN_DIR}"
    COMMAND python3
            "${CMAKE_SOURCE_DIR}/scripts/generate_web_headers.py"
            "${DASHBOARD_BUILD_DIR}"
            "${DASHBOARD_GEN_DIR}"
            --level 22
            --namespace "sgrn::datastore::assets::web"
    DEPENDS
        "${DASHBOARD_BUILD_DIR}/index.html"
        "${CMAKE_SOURCE_DIR}/scripts/generate_web_headers.py"
    COMMENT "[Dashboard] Compressing assets and generating C++ headers..."
    VERBATIM
)

add_custom_target(sgrn_dashboard_assets
    DEPENDS "${DASHBOARD_GEN_DIR}/web_assets.hpp"
)

# ── SQL Assets ───────────────────────────────────────────────────────────────
set(SQL_SRC_DIR "${PROJECT_SOURCE_DIR}/postgres")
set(SQL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/sql_generated")

file(GLOB_RECURSE SQL_DEPS "${SQL_SRC_DIR}/*.sql")

add_custom_command(
    OUTPUT "${SQL_GEN_DIR}/sql_assets.hpp"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SQL_GEN_DIR}"
    COMMAND python3
            "${CMAKE_SOURCE_DIR}/scripts/generate_embedded_assets.py"
            "${SQL_SRC_DIR}"
            "${SQL_GEN_DIR}"
            --kind "sql"
            --master "sql_assets.hpp"
            --namespace "sgrn::datastore::assets::sql"
            --level 22
    DEPENDS
        ${SQL_DEPS}
        "${CMAKE_SOURCE_DIR}/scripts/generate_embedded_assets.py"
    COMMENT "[Datastore] Compressing SQL schemas and generating C++ headers..."
    VERBATIM
)

add_custom_target(sgrn_sql_assets
    DEPENDS "${SQL_GEN_DIR}/sql_assets.hpp"
)

# ── Config Assets ────────────────────────────────────────────────────────────
set(CONFIG_SRC_DIR "${PROJECT_SOURCE_DIR}/configs")
set(CONFIG_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/config_generated")

file(GLOB_RECURSE CONFIG_DEPS "${CONFIG_SRC_DIR}/*")

add_custom_command(
    OUTPUT "${CONFIG_GEN_DIR}/config_assets.hpp"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CONFIG_GEN_DIR}"
    COMMAND python3
            "${CMAKE_SOURCE_DIR}/scripts/generate_embedded_assets.py"
            "${CONFIG_SRC_DIR}"
            "${CONFIG_GEN_DIR}"
            --kind "config"
            --master "config_assets.hpp"
            --namespace "sgrn::datastore::assets::config"
            --extensions .json .conf .service
            --exclude
                compile_commands.json
                endpoints.json
                fastcgi.conf
                fastcgi-php.conf
                snakeoil.conf
            --level 22
    DEPENDS
        ${CONFIG_DEPS}
        "${CMAKE_SOURCE_DIR}/scripts/generate_embedded_assets.py"
    COMMENT "[Datastore] Compressing default configurations..."
    VERBATIM
)

add_custom_target(sgrn_config_assets
    DEPENDS "${CONFIG_GEN_DIR}/config_assets.hpp"
)

# ── Apps ─────────────────────────────────────────────────────────────────────
add_executable(sgrn_datastore
    apps/datastore.cpp
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

# Runtime packaging
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
