# sgrn/lib/gateway/assets.cmake — embedded asset build pipelines.
# ─────────────────────────────────────────────────────────────────────────────
# Included from gateway/CMakeLists.txt before the sgrn_gateway target is
# defined (its source list references the generated headers below).
#
# Provides:
#   - ${WEB_GEN_DIR}/web_assets.hpp — Svelte SPA (sgrn/web/gateway/ → dist/)
#   - ${DOC_GEN_DIR}/doc_assets.hpp — documentation/gateway (man/markdown/txt)

# ─────────────────────────────────────────────────────────────────────────────
# Web asset generation — Svelte SPA (sgrn/web/gateway/ → dist/ → web_assets.hpp)
# ─────────────────────────────────────────────────────────────────────────────
set(WEB_SRC_DIR  "${CMAKE_SOURCE_DIR}/sgrn/web/gateway")
set(WEB_DIST_DIR "${WEB_SRC_DIR}/dist")
set(WEB_GEN_DIR  "${CMAKE_CURRENT_BINARY_DIR}/web_generated")

# ── Web UI source files tracked for rebuild ───────────────────────────────────
# Track the whole src/ tree at the new sgrn/web/gateway location (Svelte, TS,
# CSS and any future sources). CONFIGURE_DEPENDS re-globs on every build, so
# newly added or moved files are picked up automatically. Root config files are
# listed as explicit literals below: GLOB_RECURSE must NOT walk dist/ or
# node_modules/ (build artifacts would become rebuild inputs here).
file(GLOB_RECURSE WEB_SVELTE_SOURCES CONFIGURE_DEPENDS
    "${WEB_SRC_DIR}/src/*"
)

set(WEB_CONFIG_FILES
    "${WEB_SRC_DIR}/index.html"
    "${WEB_SRC_DIR}/replay.html"
    "${WEB_SRC_DIR}/vite.config.ts"
    "${WEB_SRC_DIR}/svelte.config.js"
    "${WEB_SRC_DIR}/package.json"
)
if(EXISTS "${WEB_SRC_DIR}/bun.lock")
    list(APPEND WEB_CONFIG_FILES "${WEB_SRC_DIR}/bun.lock")
endif()

# Shared TypeScript bindings used by the web app. Any change here must
# invalidate the web asset build so the embedded bundle stays in sync.
set(GATEWAY_BINDINGS_DIR "${CMAKE_SOURCE_DIR}/sgrn/typescript/gateway")
file(GLOB_RECURSE WEB_GATEWAY_BINDINGS_SOURCES CONFIGURE_DEPENDS
    "${GATEWAY_BINDINGS_DIR}/src/*.ts"
    "${GATEWAY_BINDINGS_DIR}/README.md"
    "${GATEWAY_BINDINGS_DIR}/package.json"
    "${GATEWAY_BINDINGS_DIR}/tsconfig.json"
)

# Sentinel file — touches after a successful Vite build
set(WEB_BUILD_SENTINEL "${WEB_DIST_DIR}/.build_done")

add_custom_command(
    OUTPUT  "${WEB_BUILD_SENTINEL}"
    COMMAND bun run build
    COMMAND ${CMAKE_COMMAND} -E touch "${WEB_BUILD_SENTINEL}"
    WORKING_DIRECTORY "${WEB_SRC_DIR}"
    DEPENDS
        ${WEB_SVELTE_SOURCES}
        ${WEB_CONFIG_FILES}
        ${WEB_GATEWAY_BINDINGS_SOURCES}
    COMMENT "[Web] Building Svelte SPA → dist/ via bun + Vite..."
    VERBATIM
)

# ── Glob the Vite dist and bake into web_assets.hpp ──────────────────────────
add_custom_command(
    OUTPUT  "${WEB_GEN_DIR}/web_assets.hpp"
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/scripts/generate_web_headers.py"
        "${WEB_DIST_DIR}" "${WEB_GEN_DIR}"
        --level 22 --namespace "sgrn::gateway::assets::web"
    DEPENDS "${WEB_BUILD_SENTINEL}" "${CMAKE_SOURCE_DIR}/scripts/generate_web_headers.py"
    COMMENT "[Web] Compressing Svelte assets and generating C++ headers..."
    VERBATIM
)

# ─────────────────────────────────────────────────────────────────────────────
# Doc asset generation
# ─────────────────────────────────────────────────────────────────────────────
set(DOC_SRC_DIR "${CMAKE_SOURCE_DIR}/documentation/gateway")
set(DOC_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/doc_generated")

file(GLOB_RECURSE DOC_DEPS CONFIGURE_DEPENDS "${DOC_SRC_DIR}/*")

add_custom_command(
    OUTPUT "${DOC_GEN_DIR}/doc_assets.hpp"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DOC_GEN_DIR}"
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/scripts/generate_embedded_assets.py"
            "${DOC_SRC_DIR}"
            "${DOC_GEN_DIR}"
            --kind "other"
            --master "doc_assets.hpp"
            --namespace "sgrn::gateway::assets::doc"
            --extensions .5 .md .txt
            --level 22
    DEPENDS
        ${DOC_DEPS}
        "${CMAKE_SOURCE_DIR}/scripts/generate_embedded_assets.py"
    COMMENT "[Gateway] Compressing documentation assets and generating C++ headers..."
    VERBATIM
)
