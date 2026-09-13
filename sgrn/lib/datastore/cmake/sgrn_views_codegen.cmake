# cmake/sgrn_views_codegen.cmake — compile-time CRUD view handlers.
#
# The generated `.gen.hpp` files and RegisteredViews.cpp are real, checked-in
# sources (see src/handlers/generated/). They are compiled normally; the
# ordinary build never needs DB connectivity.
#
# NOTE: the `sgrn_generate_views` target below is intentionally NOT part of
# the default build graph and does NOT run against a live database during
# `cmake --build`. It is invoked manually (like generate_orm.py) after a
# migration changes the schema; its outputs are committed to the repo.

set(VIEWS_GEN_DIR "${CMAKE_BINARY_DIR}/generated/views" CACHE INTERNAL "")

# Invoke with the DB password in the environment, e.g.:
#   PGPASSWORD=<pw> cmake --build <dir> --target sgrn_generate_views
# (the script reads PGPASSWORD itself at run time, so nothing secret is
# baked into the build files).
add_custom_target(sgrn_generate_views
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/scripts/generators/generate_views.py"
            --manifest "${PROJECT_SOURCE_DIR}/crud/manifest.json"
            --out "${PROJECT_SOURCE_DIR}/src/handlers/generated"
    COMMENT "Regenerating CRUD view handlers from live schema + crud/manifest.json (manual step; needs PGPASSWORD in the environment)"
)

# src/query/ holds the hand-written runtime engine (CrudViewEngine.cpp).
# It is not covered by the HANDLERS/SERVICES/FILTERS/PLUGINS globs, so add it
# explicitly. (src/handlers/generated/RegisteredViews.cpp IS covered by the
# HANDLERS_SOURCES glob, which recurses into subdirectories.)
file(GLOB SGRN_QUERY_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/query/*.cpp")
target_sources(sgrn_datastore_lib PRIVATE ${SGRN_QUERY_SOURCES})
