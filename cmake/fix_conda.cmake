# ─────────────────────────────────────────────────────────────────────────────
# fix_conda.cmake — Robust path repair for Windows Conda environments.
#
# Some Conda packages on Windows (like ZLIB, OpenSSL) ship broken .cmake or .pc
# files that use "/Library/include" or "/Library/lib" notation. On Windows, 
# these resolve to the drive root (X:\Library) instead of the prefix.
#
# This module implements "Target Repair" to fix these paths on the fly.
# ─────────────────────────────────────────────────────────────────────────────

include_guard(GLOBAL)

function(sgrn_repair_target TARGET_NAME)
    if(NOT TARGET ${TARGET_NAME})
        return()
    endif()

    # Resolve ALIAS targets recursively (cannot set properties on them directly)
    while(TRUE)
        set(_alias "")
        get_target_property(_alias_for ${TARGET_NAME} ALIAS_FOR)
        if(_alias_for AND NOT _alias_for MATCHES "-NOTFOUND$")
            set(_alias "${_alias_for}")
        else()
            get_target_property(_aliased ${TARGET_NAME} ALIASED_TARGET)
            if(_aliased AND NOT _aliased MATCHES "-NOTFOUND$")
                set(_alias "${_aliased}")
            endif()
        endif()

        if(_alias)
            message(STATUS "[SGRN/Repair]   Resolving ALIAS ${TARGET_NAME} -> ${_alias}")
            set(TARGET_NAME "${_alias}")
        else()
            break()
        endif()
    endwhile()

    if(NOT SGRN_CONDA_PREFIX)
        return()
    endif()

    message(STATUS "[SGRN/Repair] Inspecting target ${TARGET_NAME}")

    # 1. Repair INTERFACE_INCLUDE_DIRECTORIES
    get_target_property(_inc_dirs ${TARGET_NAME} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc_dirs)
        set(_new_inc_dirs "")
        foreach(_dir ${_inc_dirs})
            if(_dir MATCHES "^/Library")
                set(_abs_dir "${SGRN_CONDA_PREFIX}${_dir}")
                list(APPEND _new_inc_dirs "${_abs_dir}")
                message(STATUS "[SGRN/Repair]   Fixed Include: ${_dir} -> ${_abs_dir}")
            else()
                list(APPEND _new_inc_dirs "${_dir}")
            endif()
        endforeach()
        set_target_properties(${TARGET_NAME} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_new_inc_dirs}")
    endif()

    # 2. Repair IMPORTED_LOCATION (and variants)
    foreach(_prop IMPORTED_LOCATION IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_DEBUG)
        get_target_property(_loc ${TARGET_NAME} ${_prop})
        if(_loc AND _loc MATCHES "^/Library")
            set(_abs_loc "${SGRN_CONDA_PREFIX}${_loc}")
            set_target_properties(${TARGET_NAME} PROPERTIES ${_prop} "${_abs_loc}")
            message(STATUS "[SGRN/Repair]   Fixed Location: ${_loc} -> ${_abs_loc}")
        endif()
    endforeach()

    # 3. Repair INTERFACE_LINK_LIBRARIES (recursive target references)
    get_target_property(_link_libs ${TARGET_NAME} INTERFACE_LINK_LIBRARIES)
    if(_link_libs)
        set(_new_link_libs "")
        foreach(_lib ${_link_libs})
            if(_lib MATCHES "^/Library")
                set(_abs_lib "${SGRN_CONDA_PREFIX}${_lib}")
                list(APPEND _new_link_libs "${_abs_lib}")
                message(STATUS "[SGRN/Repair]   Fixed Link Lib: ${_lib} -> ${_abs_lib}")
            else()
                list(APPEND _new_link_libs "${_lib}")
            endif()
        endforeach()
        set_target_properties(${TARGET_NAME} PROPERTIES INTERFACE_LINK_LIBRARIES "${_new_link_libs}")
    endif()

endfunction()
