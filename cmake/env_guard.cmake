# cmake/env_guard.cmake — Fail fast (BEFORE project()) when outside SGRN env
# ─────────────────────────────────────────────────────────────────────────────
# The SGRN presets assume a micromamba environment named "SGRN" provides the
# build toolchain and the target dependency libraries, and they key search
# paths off $CONDA_PREFIX. Because `project()` immediately needs a working
# compiler, we validate the environment HERE — before `project()` runs — so a
# forgotten activation yields a clear, actionable error instead of an obscure
# "CMAKE_CXX_COMPILER not found in PATH" or a cascade of missing dependencies.
#
# Intended usage (see README.md / documentation/BUILD.md):
#     micromamba activate SGRN
#     cmake --preset <preset>            # then: cmake --build <dir> --target install
# ─────────────────────────────────────────────────────────────────────────────

if(NOT DEFINED ENV{CONDA_PREFIX} OR "$ENV{CONDA_PREFIX}" STREQUAL "")
    message(FATAL_ERROR
"[SGRN] No conda (micromamba) environment is active.

The SGRN build must run from inside the 'SGRN' conda environment, which
provides the compilers, ninja and the target dependency libraries. Activate
it first, then re-run the exact same command:

    micromamba activate SGRN
    <re-run your cmake command>

(if your manager is conda rather than micromamba:  conda activate SGRN)

Do not skip this step: the presets hardcode tool paths into this environment
and dependency discovery is keyed off $CONDA_PREFIX.")
endif()

# A gentler warning when a *different* env happens to be active (the usual cause
# of hard-to-debug "package/library not found" errors).
if(DEFINED ENV{CONDA_PREFIX})
    get_filename_component(_sgrn_conda_name "$ENV{CONDA_PREFIX}" NAME)
    if(NOT _sgrn_conda_name STREQUAL "SGRN")
        message(WARNING
"[SGRN] The active conda environment is '${_sgrn_conda_name}', not 'SGRN'.
The SGRN presets are built and tested against the 'SGRN' environment. If you
run into missing compilers or dependency libraries, switch to it:

    micromamba activate SGRN")
    endif()
endif()
