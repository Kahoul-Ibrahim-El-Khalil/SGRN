# cmake/sgrn_views_codegen.cmake — compile-time CRUD view handlers & ORM codegen.
#
# The generated `.gen.hpp` files, RegisteredViews.cpp, and ORM models are
# real, checked-in sources (see src/handlers/generated/ and src/orm/models/).
# They are compiled normally.
#
# When SQL schemas or manifest change, this automatically triggers regeneration
# against an isolated build database via `generate_orm.py --build-db --views`.

set(VIEWS_GEN_DIR "${CMAKE_BINARY_DIR}/generated/views" CACHE INTERNAL "")

option(SGRN_AUTO_CODEGEN "Automatically regenerate ORM and CRUD views when SQL schemas or manifest change" ON)

set(SGRN_MANIFEST_FILE "${PROJECT_SOURCE_DIR}/crud/manifest.json")
file(GLOB_RECURSE SGRN_SQL_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/postgres/*.sql")
set(SGRN_ORM_GEN_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/generators/generate_orm.py")
set(SGRN_VIEWS_GEN_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/generators/generate_views.py")
set(SGRN_INIT_DB_SCRIPT "${CMAKE_SOURCE_DIR}/init-db.py")
set(SGRN_FORMAT_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/format.py")
set(SGRN_CODEGEN_STAMP "${CMAKE_CURRENT_BINARY_DIR}/sgrn_codegen.stamp")

find_program(CLANG_FORMAT_BIN NAMES clang-format clang-format-22 clang-format-21 clang-format-20 clang-format-19
    HINTS
        "${CMAKE_CXX_COMPILER}/../"
        "${CMAKE_CXX_COMPILER}/../../bin"
        "$ENV{CONDA_PREFIX}/bin"
        "$ENV{HOME}/micromamba/envs/SGRN/bin"
        "$ENV{HOME}/.micromamba/envs/SGRN/bin"
)

set(SGRN_FORMAT_ARGS cpp --dir "${PROJECT_SOURCE_DIR}/src/orm/models" --dir "${PROJECT_SOURCE_DIR}/src/handlers/generated")
if(CLANG_FORMAT_BIN)
    list(APPEND SGRN_FORMAT_ARGS --clang-format "${CLANG_FORMAT_BIN}")
endif()

if(SGRN_AUTO_CODEGEN)
    add_custom_command(
        OUTPUT "${SGRN_CODEGEN_STAMP}"
        COMMAND "${Python3_EXECUTABLE}" "${SGRN_ORM_GEN_SCRIPT}" --build-db --views
        COMMAND "${Python3_EXECUTABLE}" "${SGRN_FORMAT_SCRIPT}" ${SGRN_FORMAT_ARGS}
        COMMAND "${CMAKE_COMMAND}" -E touch "${SGRN_CODEGEN_STAMP}"
        DEPENDS
            "${SGRN_MANIFEST_FILE}"
            ${SGRN_SQL_FILES}
            "${SGRN_ORM_GEN_SCRIPT}"
            "${SGRN_VIEWS_GEN_SCRIPT}"
            "${SGRN_INIT_DB_SCRIPT}"
            "${SGRN_FORMAT_SCRIPT}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "SQL schemas or manifest changed: regenerating Drogon ORM models and CRUD views via isolated build database and formatting generated C++ code..."
        VERBATIM
    )

    add_custom_target(sgrn_codegen ALL
        DEPENDS "${SGRN_CODEGEN_STAMP}"
    )

    if(TARGET sgrn_datastore_orm)
        add_dependencies(sgrn_datastore_orm sgrn_codegen)
    endif()
    if(TARGET sgrn_datastore_lib)
        add_dependencies(sgrn_datastore_lib sgrn_codegen)
    endif()
endif()

# Manual regeneration target
add_custom_target(sgrn_generate_views
    COMMAND "${Python3_EXECUTABLE}" "${SGRN_ORM_GEN_SCRIPT}" --build-db --views
    COMMAND "${Python3_EXECUTABLE}" "${SGRN_FORMAT_SCRIPT}" ${SGRN_FORMAT_ARGS}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Manual regeneration of Drogon ORM models and CRUD views via isolated build database with formatting"
)

# src/query/ holds the hand-written runtime engine (CrudViewEngine.cpp).
# It is not covered by the HANDLERS/SERVICES/FILTERS/PLUGINS globs, so add it
# explicitly. (src/handlers/generated/RegisteredViews.cpp IS covered by the
# HANDLERS_SOURCES glob, which recurses into subdirectories.)
file(GLOB SGRN_QUERY_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/query/*.cpp")
target_sources(sgrn_datastore_lib PRIVATE ${SGRN_QUERY_SOURCES})
