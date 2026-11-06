# cmake/deps/reproc.cmake — Exposes: reproc::reproc, reproc::reproc++
# ─────────────────────────────────────────────────────────────────────────────
# reproc (Redirected Process) — cross-platform C99/C++11 child-process
# control: start with cwd/env-extend/redirect-to-file, wait with timeout, and
# a shutdown escalation chain (stop → terminate → kill). Used by the shared
# C++ test-support fixtures (sgrn/lib/tests/support/test-util.hpp) so they no
# longer depend on raw fork/exec/waitpid (and therefore stay portable to
# Windows when the test targets there are unblocked).
#
# Like the other CPM deps, this is fetched via sgrn_fetch_source() (source
# only) and built with plain add_subdirectory + EXCLUDE_FROM_ALL, so it is
# compiled exactly when a test target links it.

if(TARGET reproc++)
    return()
endif()

sgrn_fetch_source(reproc)

# Only need the C core + C++11 wrapper; no develop/QA targets, no install
# rules, and match BUILD_SHARED_LIBS across the build.
set(REPROC++             ON  CACHE BOOL "" FORCE)
set(REPROC_DEVELOP       OFF CACHE BOOL "" FORCE)
set(REPROC_TEST          OFF CACHE BOOL "" FORCE)
set(REPROC_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(REPROC_WARNINGS      OFF CACHE BOOL "" FORCE)
set(REPROC_INSTALL       OFF CACHE BOOL "" FORCE)
set(REPROC_INSTALL_PKGCONFIG OFF CACHE BOOL "" FORCE)
add_subdirectory("${reproc_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/reproc" EXCLUDE_FROM_ALL)

# reproc does not export namespaced aliases in build-tree mode; provide the
# canonical :: targets so test targets link reproc::reproc++ only.
if(NOT TARGET reproc::reproc)
    add_library(reproc::reproc ALIAS reproc)
endif()
if(NOT TARGET reproc::reproc++)
    add_library(reproc::reproc++ ALIAS reproc++)
endif()
