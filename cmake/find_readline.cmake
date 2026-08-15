# find_readline.cmake — Shared readline/history discovery for SGRN components
# Usage: include this file, then call sgrn_find_readline().
# After the call, SGRN_READLINE_LIBS contains the libs to link.
include_guard(GLOBAL)

macro(sgrn_find_readline)
    if(UNIX)
        # Try finding the absolute paths from Conda prefix first
        if(SGRN_CONDA_PREFIX)
            find_library(SGRN_READLINE_LIB NAMES readline
                PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
            find_library(SGRN_HISTORY_LIB  NAMES history
                PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
        endif()

        if(SGRN_READLINE_LIB)
            set(SGRN_READLINE_LIBS ${SGRN_READLINE_LIB} ${SGRN_HISTORY_LIB})
        else()
            find_package(PkgConfig QUIET)
            if(PKG_CONFIG_FOUND)
                pkg_check_modules(READLINE QUIET readline)
            endif()

            if(READLINE_FOUND)
                find_library(SGRN_READLINE_LIB NAMES readline HINTS ${READLINE_LIBRARY_DIRS})
                find_library(SGRN_HISTORY_LIB NAMES history HINTS ${READLINE_LIBRARY_DIRS})
                if(SGRN_READLINE_LIB)
                    set(SGRN_READLINE_LIBS ${SGRN_READLINE_LIB} ${SGRN_HISTORY_LIB})
                else()
                    set(SGRN_READLINE_LIBS ${READLINE_LIBRARIES})
                endif()
            else()
                find_library(SGRN_READLINE_LIB NAMES readline)
                find_library(SGRN_HISTORY_LIB  NAMES history)
                if(SGRN_READLINE_LIB)
                    set(SGRN_READLINE_LIBS ${SGRN_READLINE_LIB} ${SGRN_HISTORY_LIB})
                else()
                    # Fallback: rely on system linker to find readline
                    set(SGRN_READLINE_LIBS readline history)
                endif()
            endif()
        endif()
    else()
        set(SGRN_READLINE_LIBS "")
    endif()
endmacro()
