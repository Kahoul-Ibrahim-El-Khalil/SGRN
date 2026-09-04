# cmake/deps/FetchDeps.cmake — CPM.cmake-based dependency declarations.
# ─────────────────────────────────────────────────────────────────────────────
# CPM.cmake adds a persistent source cache (CPM_SOURCE_CACHE, default ~/.cache/CPM)
# that survives `rm -rf .build/` — sources are never re-downloaded once cached.
# CPM_SOURCE_CACHE is set to $HOME/.cache/CPM in CMakePresets.json (base-common).
#
# Deps with GitHub release archives use URL tarballs (faster: no git protocol).
# Deps without release archives use shallow git clones as fallback.
#
# NOTE:
# URL_HASH verification is intentionally omitted here for development
# convenience. To harden for production, add SHA256 hashes after computing them:
#
#   cmake -E sha256sum <downloaded-tarball>
#
# DO NOT add CPMAddPackage() calls anywhere else in the build system.
# ─────────────────────────────────────────────────────────────────────────────

include_guard(GLOBAL)

# ── Bootstrap CPM.cmake ───────────────────────────────────────────────────────

set(_cpm_version "0.43.1")

if(DEFINED ENV{CPM_SOURCE_CACHE})
    set(_cpm_dir "$ENV{CPM_SOURCE_CACHE}/cpm")
elseif(DEFINED CPM_SOURCE_CACHE)
    set(_cpm_dir "${CPM_SOURCE_CACHE}/cpm")
else()
    set(_cpm_dir "${CMAKE_SOURCE_DIR}/.build/_deps/cpm")
endif()

set(_cpm_path "${_cpm_dir}/CPM_${_cpm_version}.cmake")

if(NOT EXISTS "${_cpm_path}")

    message(STATUS
        "[SGRN] Downloading CPM.cmake v${_cpm_version} → ${_cpm_path}"
    )

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

        file(REMOVE "${_cpm_path}")

        message(FATAL_ERROR
            "[SGRN] Failed to download CPM.cmake (${_cpm_msg}). "
            "Place CPM_${_cpm_version}.cmake manually at:\n  ${_cpm_path}\n"
            "Download: https://github.com/cpm-cmake/CPM.cmake/releases/v${_cpm_version}"
        )

    endif()

    # Sanity check against HTML/error pages.
    file(READ "${_cpm_path}" _cpm_content LIMIT 80)

    if(NOT _cpm_content MATCHES "cmake_minimum_required|CPM")

        file(REMOVE "${_cpm_path}")

        message(FATAL_ERROR
            "[SGRN] Downloaded file does not look like CPM.cmake "
            "(got HTML redirect?). Deleted corrupt file. Re-run cmake to retry."
        )

    endif()

endif()

include("${_cpm_path}")

# ── Dependency metadata ───────────────────────────────────────────────────────
#
# _URL       → GitHub release tarball
# _REPO/_TAG → shallow git clone
#
# sgrn_fetch_source() resolves these lazily — a dependency is downloaded only
# when the corresponding source is actually requested.
# ─────────────────────────────────────────────────────────────────────────────

# fmt 12.2.0
set(SGRN_DEP_fmt_URL
    "https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz"
)

# jsoncpp 1.9.8
set(SGRN_DEP_jsoncpp_URL
    "https://github.com/open-source-parsers/jsoncpp/archive/refs/tags/1.9.8.tar.gz"
)

# Snap7 1.4.3 upstream baseline.
#
# Snap7's official GitHub repository currently starts with release 1.4.3.
# SGRN pins an immutable upstream commit and performs the required hardening
# locally during dependency acquisition rather than tracking mutable main.
set(SGRN_DEP_snap7_REPO
    "https://github.com/davenardella/snap7.git"
)

set(SGRN_DEP_snap7_TAG
    "30f37da3114024a71ba93f7fd855c680b97a406f"
)

# open62541 v1.5.7
set(SGRN_DEP_open62541_URL
    "https://github.com/open62541/open62541/archive/refs/tags/v1.5.7.tar.gz"
)

# AngelScript 2.38.0 — official upstream.
set(SGRN_DEP_angelscript_REPO
    "https://github.com/anjo76/angelscript.git"
)

set(SGRN_DEP_angelscript_TAG
    "v2.38.0"
)

# IXWebSocket v12.0.1
set(SGRN_DEP_ixwebsocket_URL
    "https://github.com/machinezone/IXWebSocket/archive/refs/tags/v12.0.1.tar.gz"
)

# libmodbus v3.2.0
set(SGRN_DEP_libmodbus_URL
    "https://github.com/stephane/libmodbus/archive/refs/tags/v3.2.0.tar.gz"
)

# OpENer master branch
set(SGRN_DEP_opener_REPO
    "https://github.com/EIPStackGroup/OpENer.git"
)

set(SGRN_DEP_opener_TAG
    "master"
)

# Drogon v1.9.13
set(SGRN_DEP_drogon_REPO
    "https://github.com/drogonframework/drogon.git"
)

set(SGRN_DEP_drogon_TAG
    "v1.9.13"
)

# RapidJSON master snapshot — fixes C++20/23 standard compliance issues present in 1.1.0 release.
set(SGRN_DEP_rapidjson_URL
    "https://github.com/Tencent/rapidjson/archive/refs/heads/master.tar.gz"
)

# cpp-httplib v0.54.1
set(SGRN_DEP_cpp_httplib_URL
    "https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.54.1.tar.gz"
)

# xml.h 2.1
set(SGRN_DEP_xml_h_REPO
    "https://github.com/mrvladus/xml.h.git"
)

set(SGRN_DEP_xml_h_TAG
    "2.1"
)

# sqlite_modern_cpp v3.2
set(SGRN_DEP_sqlite_modern_cpp_URL
    "https://github.com/SqliteModernCpp/sqlite_modern_cpp/archive/refs/tags/v3.2.tar.gz"
)

# unordered_dense v4.9.2
set(SGRN_DEP_unordered_dense_URL
    "https://github.com/martinus/unordered_dense/archive/refs/tags/v4.9.2.tar.gz"
)

# ── Snap7 inline hardening ────────────────────────────────────────────────────
#
# Applies the client-side PDU-length hardening directly in the fetched source.
#
# No companion patch file is required.
#
# The source transformation is intentionally performed after acquisition and
# before the dependency is consumed by the rest of the build.
#
# IMPORTANT:
# This is an SGRN-local source hardening of the pinned upstream Snap7 tree.
# ─────────────────────────────────────────────────────────────────────────────

function(sgrn_harden_snap7 SOURCE_DIR)

    if(NOT EXISTS "${SOURCE_DIR}/src/core/s7_peer.cpp")
        message(FATAL_ERROR
            "[SGRN] Snap7 hardening failed: "
            "src/core/s7_peer.cpp was not found in:\n  ${SOURCE_DIR}"
        )
    endif()

    file(READ
        "${SOURCE_DIR}/src/core/s7_peer.cpp"
        _snap7_peer_source
    )

    string(FIND
        "${_snap7_peer_source}"
        "PDULength = SwapWord(ResNegotiate->PDULength);"
        _snap7_pdulen_pos
    )

    if(_snap7_pdulen_pos EQUAL -1)

        message(FATAL_ERROR
            "[SGRN] Snap7 hardening failed: "
            "expected vulnerable PDULength assignment was not found. "
            "The pinned source layout may have changed."
        )

    endif()

    string(REPLACE
        "PDULength = SwapWord(ResNegotiate->PDULength);"

        "word NegotiatedPDULength = SwapWord(ResNegotiate->PDULength);\n"
        "    if (NegotiatedPDULength > IsoPayload_Size)\n"
        "        NegotiatedPDULength = IsoPayload_Size;\n"
        "    PDULength = NegotiatedPDULength;"

        _snap7_peer_patched
        "${_snap7_peer_source}"
    )

    if(_snap7_peer_patched STREQUAL _snap7_peer_source)

        message(FATAL_ERROR
            "[SGRN] Snap7 hardening failed: "
            "source transformation produced no change."
        )

    endif()

    file(WRITE
        "${SOURCE_DIR}/src/core/s7_peer.cpp"
        "${_snap7_peer_patched}"
    )

    message(STATUS
        "[SGRN] Snap7 client PDU-length hardening applied."
    )

endfunction()

# ─────────────────────────────────────────────────────────────────────────────
# Source fetch helper
# ─────────────────────────────────────────────────────────────────────────────
#
# sgrn_fetch_source(<name>)
#
# Downloads the dependency if it is not already available in CPM_SOURCE_CACHE
# and sets <name>_SOURCE_DIR.
#
# URL tarballs are preferred because they avoid git protocol overhead.
# Dependencies without a URL fall back to shallow git clones.
#
# Snap7 receives its security hardening automatically.
#
# Idempotent — CPM deduplicates repeated requests for the same package.
# ─────────────────────────────────────────────────────────────────────────────

macro(sgrn_fetch_source DEP_NAME)

    if(NOT ${DEP_NAME}_SOURCE_DIR)

        set(_sfs_url
            "${SGRN_DEP_${DEP_NAME}_URL}"
        )

        set(_sfs_repo
            "${SGRN_DEP_${DEP_NAME}_REPO}"
        )

        set(_sfs_tag
            "${SGRN_DEP_${DEP_NAME}_TAG}"
        )

        if(_sfs_url)

            CPMAddPackage(
                NAME           ${DEP_NAME}
                URL            "${_sfs_url}"
                DOWNLOAD_ONLY  YES
            )

        elseif(_sfs_repo)

            set(_shallow TRUE)
            if(_sfs_tag MATCHES "^[0-9a-fA-F]{40}$")
                set(_shallow FALSE)
            endif()

            CPMAddPackage(
                NAME           ${DEP_NAME}
                GIT_REPOSITORY "${_sfs_repo}"
                GIT_TAG        "${_sfs_tag}"
                GIT_SHALLOW    ${_shallow}
                DOWNLOAD_ONLY  YES
            )

        else()

            message(FATAL_ERROR
                "[SGRN] sgrn_fetch_source(${DEP_NAME}): no metadata found. "
                "Add SGRN_DEP_${DEP_NAME}_URL or _REPO/TAG to "
                "cmake/deps/FetchDeps.cmake"
            )

        endif()

        # Snap7 is the only dependency requiring local source hardening.
        if(DEP_NAME STREQUAL "snap7")

            if(NOT snap7_SOURCE_DIR)

                message(FATAL_ERROR
                    "[SGRN] Snap7 was requested but CPM did not provide "
                    "snap7_SOURCE_DIR."
                )

            endif()

            sgrn_harden_snap7("${snap7_SOURCE_DIR}")

        endif()

    endif()

endmacro()
