# cmake/deps/FetchDeps.cmake — CPM.cmake-based dependency declarations.
# ─────────────────────────────────────────────────────────────────────────────
# CPM.cmake adds a persistent source cache (CPM_SOURCE_CACHE, default ~/.cache/CPM)
# that survives `rm -rf .build/` — sources are never re-downloaded once cached.
# CPM_SOURCE_CACHE is set to $HOME/.cache/CPM in CMakePresets.json (base-common).
#
# Deps with GitHub release archives use URL tarballs (faster: no git protocol).
# Deps without release archives use shallow git clones as fallback.
#
# NOTE: URL_HASH verification is intentionally omitted here for development
# convenience. To harden for production, add SHA256 hashes after computing them:
#   cmake -E sha256sum <downloaded-tarball>
#
# DO NOT add CPMAddPackage() calls anywhere else in the build system.
# ─────────────────────────────────────────────────────────────────────────────

include_guard(GLOBAL)

# ── Bootstrap CPM.cmake ───────────────────────────────────────────────────────
set(_cpm_version "0.42.0")
if(DEFINED ENV{CPM_SOURCE_CACHE})
    set(_cpm_dir "$ENV{CPM_SOURCE_CACHE}/cpm")
elseif(DEFINED CPM_SOURCE_CACHE)
    set(_cpm_dir "${CPM_SOURCE_CACHE}/cpm")
else()
    set(_cpm_dir "${CMAKE_SOURCE_DIR}/.build/_deps/cpm")
endif()
set(_cpm_path "${_cpm_dir}/CPM_${_cpm_version}.cmake")

if(NOT EXISTS "${_cpm_path}")
    message(STATUS "[SGRN] Downloading CPM.cmake v${_cpm_version} → ${_cpm_path}")
    # Create the cache directory — file(DOWNLOAD) does not create missing parents.
    file(MAKE_DIRECTORY "${_cpm_dir}")
    file(DOWNLOAD
        "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${_cpm_version}/CPM.cmake"
        "${_cpm_path}"
        TIMEOUT 60
        SHOW_PROGRESS
        STATUS _cpm_status
    )
    list(GET _cpm_status 0 _cpm_error)
    list(GET _cpm_status 1 _cpm_msg)
    if(_cpm_error)
        file(REMOVE "${_cpm_path}")   # remove partial/corrupt download
        message(FATAL_ERROR
            "[SGRN] Failed to download CPM.cmake (${_cpm_msg}). "
            "Place CPM_${_cpm_version}.cmake manually at:\n  ${_cpm_path}\n"
            "Download: https://github.com/cpm-cmake/CPM.cmake/releases/v${_cpm_version}")
    endif()
    # Sanity check: a valid CPM.cmake starts with cmake_minimum_required, not HTML
    file(READ "${_cpm_path}" _cpm_content LIMIT 80)
    if(NOT _cpm_content MATCHES "cmake_minimum_required|CPM")
        file(REMOVE "${_cpm_path}")
        message(FATAL_ERROR
            "[SGRN] Downloaded file does not look like CPM.cmake (got HTML redirect?). "
            "Deleted corrupt file. Re-run cmake to retry.")
    endif()
endif()
include("${_cpm_path}")

# ── Dep metadata ──────────────────────────────────────────────────────────────
# _URL  → GitHub release tarball (fast HTTP download, no git overhead)
# _REPO/_TAG → shallow git clone (for deps without release archives)
# sgrn_fetch_source() reads these lazily — only downloads when called.

# fmt 12.1.0
set(SGRN_DEP_fmt_URL    "https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz")

# jsoncpp 1.9.8
set(SGRN_DEP_jsoncpp_URL "https://github.com/open-source-parsers/jsoncpp/archive/refs/tags/1.9.8.tar.gz")

# snap7 — no release tags, shallow git clone
set(SGRN_DEP_snap7_REPO "https://github.com/davenardella/snap7.git")
set(SGRN_DEP_snap7_TAG  "main")

# open62541 v1.4.15 (LTS)
set(SGRN_DEP_open62541_URL "https://github.com/open62541/open62541/archive/refs/tags/v1.4.15.tar.gz")

# angelscript — IngwiePhoenix CMake fork, no release tags
set(SGRN_DEP_angelscript_REPO "https://github.com/IngwiePhoenix/AngelScript.git")
set(SGRN_DEP_angelscript_TAG  "50229f5281b40dafa22b7c135a499798f39ddb3f")

# ixwebsocket v12.0.1
set(SGRN_DEP_ixwebsocket_URL "https://github.com/machinezone/IXWebSocket/archive/refs/tags/v12.0.1.tar.gz")

# libmodbus v3.1.12
set(SGRN_DEP_libmodbus_URL "https://github.com/stephane/libmodbus/archive/refs/tags/v3.1.12.tar.gz")

# opener — last release v2.3 (2019), master has maintenance
set(SGRN_DEP_opener_REPO "https://github.com/EIPStackGroup/OpENer.git")
set(SGRN_DEP_opener_TAG  "master")

# drogon v1.9.13 — tarball doesn't include trantor submodule, must use git
set(SGRN_DEP_drogon_REPO "https://github.com/drogonframework/drogon.git")
set(SGRN_DEP_drogon_TAG  "v1.9.13")

# rapidjson — last release 2016 (v1.1.0), master has fixes, shallow git
set(SGRN_DEP_rapidjson_REPO "https://github.com/Tencent/rapidjson.git")
set(SGRN_DEP_rapidjson_TAG  "master")

# cpp-httplib v0.53.0
set(SGRN_DEP_cpp_httplib_URL "https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.53.0.tar.gz")

# xml_h v2.1 (repo tags only, no release tarballs)
set(SGRN_DEP_xml_h_REPO "https://github.com/mrvladus/xml.h.git")
set(SGRN_DEP_xml_h_TAG  "2.1")

# sqlite_modern_cpp v3.2
set(SGRN_DEP_sqlite_modern_cpp_URL "https://github.com/SqliteModernCpp/sqlite_modern_cpp/archive/refs/tags/v3.2.tar.gz")

# unordered_dense v4.4.0
set(SGRN_DEP_unordered_dense_URL "https://github.com/martinus/unordered_dense/archive/refs/tags/v4.4.0.tar.gz")

# ─────────────────────────────────────────────────────────────────────────────
# sgrn_fetch_source(<name>)
# Downloads the dep (if not in CPM_SOURCE_CACHE) and sets <name>_SOURCE_DIR.
# Prefers URL tarballs; falls back to shallow git clone when no URL is set.
# Idempotent — CPM deduplicates multiple calls for the same package.
# ─────────────────────────────────────────────────────────────────────────────
macro(sgrn_fetch_source DEP_NAME)
    if(NOT ${DEP_NAME}_SOURCE_DIR)
        set(_sfs_url  "${SGRN_DEP_${DEP_NAME}_URL}")
        set(_sfs_repo "${SGRN_DEP_${DEP_NAME}_REPO}")
        set(_sfs_tag  "${SGRN_DEP_${DEP_NAME}_TAG}")

        if(_sfs_url)
            CPMAddPackage(NAME ${DEP_NAME} URL "${_sfs_url}" DOWNLOAD_ONLY YES)
        elseif(_sfs_repo)
            CPMAddPackage(
                NAME           ${DEP_NAME}
                GIT_REPOSITORY "${_sfs_repo}"
                GIT_TAG        "${_sfs_tag}"
                GIT_SHALLOW    TRUE
                DOWNLOAD_ONLY  YES
            )
        else()
            message(FATAL_ERROR
                "[SGRN] sgrn_fetch_source(${DEP_NAME}): no metadata found. "
                "Add SGRN_DEP_${DEP_NAME}_URL or _REPO/TAG to cmake/deps/FetchDeps.cmake")
        endif()
    endif()
endmacro()
