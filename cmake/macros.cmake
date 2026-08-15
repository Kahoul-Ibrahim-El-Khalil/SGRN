# macros.cmake — Shared SGRN CMake utility macros
# Single source of truth — included by global.cmake; do NOT duplicate elsewhere.
include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# sgrn_add_to_install — Register a non-imported target for installation
# ---------------------------------------------------------------------------
macro(sgrn_add_to_install _target)
    if(TARGET ${_target})
        get_target_property(_imported ${_target} IMPORTED)
        if(NOT _imported)
            list(APPEND SGRN_INSTALL_TARGETS ${_target})
            set(SGRN_INSTALL_TARGETS "${SGRN_INSTALL_TARGETS}" CACHE INTERNAL "" FORCE)
        endif()
    endif()
endmacro()

# ---------------------------------------------------------------------------
# sgrn_alias — Declare a stable sgrn::* INTERFACE alias target
# ---------------------------------------------------------------------------
macro(sgrn_alias NAME)
    if(NOT TARGET ${NAME})
        add_library(${NAME} INTERFACE IMPORTED GLOBAL)
    endif()
endmacro()

# ---------------------------------------------------------------------------
# sgrn_link_dependency — Wire an alias to a real (found or built) target
# ---------------------------------------------------------------------------
macro(sgrn_link_dependency ALIAS_NAME TARGET_VAR ACTUAL_TARGET)
    if(TARGET ${ALIAS_NAME} AND (TARGET ${ACTUAL_TARGET} OR ${TARGET_VAR}_FOUND))
        target_link_libraries(${ALIAS_NAME} INTERFACE ${ACTUAL_TARGET})
    endif()
endmacro()

# ---------------------------------------------------------------------------
# sgrn_repair_target — Windows import-library / DLL location fixup
# ---------------------------------------------------------------------------
macro(sgrn_repair_target _target)
    if(TARGET ${_target})
        get_target_property(_imported ${_target} IMPORTED)
        if(_imported AND (WIN32 OR SGRN_CROSS_WIN64))
            get_target_property(_loc ${_target} IMPORTED_LOCATION)
            if(_loc AND (_loc MATCHES ".lib$" OR _loc MATCHES ".dll.a$"))
                set_target_properties(${_target} PROPERTIES IMPORTED_IMPLIB "${_loc}")
                get_filename_component(_dir  "${_loc}" DIRECTORY)
                get_filename_component(_name "${_loc}" NAME_WLE)
                if(EXISTS "${_dir}/${_name}.dll")
                    set_target_properties(${_target} PROPERTIES IMPORTED_LOCATION "${_dir}/${_name}.dll")
                elseif(EXISTS "${_dir}/../bin/${_name}.dll")
                    set_target_properties(${_target} PROPERTIES IMPORTED_LOCATION "${_dir}/../bin/${_name}.dll")
                endif()
            endif()
        endif()
    endif()
endmacro()

# ---------------------------------------------------------------------------
# sgrn_assert_wired — Fail at configure time if a required dependency alias was
# declared but never populated. An empty alias silently yields link-time
# undefined-reference errors (e.g. the historical win64 fmt bug); this surfaces
# that immediately instead of at link time.
# ---------------------------------------------------------------------------
function(sgrn_assert_wired _alias)
    if(TARGET ${_alias})
        get_target_property(_links ${_alias} INTERFACE_LINK_LIBRARIES)
        if(NOT _links OR _links STREQUAL "")
            message(FATAL_ERROR
"[SGRN] Dependency alias '${_alias}' was declared but never wired to a provider.

No dependency provides it in this configuration, so any consumer will fail at
link time with undefined references. Fix the provider wiring for ${_alias} (see
cmake/deps*.cmake and the sgrn_link_dependency() calls in the root CMakeLists).")
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# sgrn_assert_dependencies_wired — Assert every entry of the process-wide
# SGRN_REQUIRED_DEPENDENCIES list is populated. Call AFTER all dependency
# wiring (deps.cmake + sgrn_link_dependency()) has run.
# ---------------------------------------------------------------------------
function(sgrn_assert_dependencies_wired)
    foreach(_dep IN LISTS SGRN_REQUIRED_DEPENDENCIES)
        sgrn_assert_wired(sgrn::${_dep})
    endforeach()
endfunction()

