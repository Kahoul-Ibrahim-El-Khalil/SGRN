# harvest_linux.cmake — Native Linux SO dependency puller
# Called at install time via: install(SCRIPT cmake/scripts/harvest_linux.cmake)
# Variables injected by packaging.cmake via set_property(... APPEND ...):
#   SGRN_HARVEST_EXECUTABLES, SGRN_HARVEST_LIBRARIES, SGRN_HARVEST_SEARCH_DIRS

message(STATUS "[SGRN-PKG] Pulling native Linux artifacts...")

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${SGRN_HARVEST_EXECUTABLES}
    LIBRARIES   ${SGRN_HARVEST_LIBRARIES}
    RESOLVED_DEPENDENCIES_VAR _resolved_deps
    DIRECTORIES ${SGRN_HARVEST_SEARCH_DIRS}
    PRE_EXCLUDE_REGEXES  "^/lib/" "^/lib64/" "^/usr/lib/" "^/usr/bin/"
    POST_EXCLUDE_REGEXES "^/lib/" "^/lib64/" "^/usr/lib/" "^/usr/bin/"
)

# Whitelist pattern: only pull deps that came from our managed prefixes
set(_managed_pattern "micromamba|prefix|fmt|jsoncpp|snap7|angelscript|open62541")

foreach(_dep ${_resolved_deps})
    if(_dep MATCHES "${_managed_pattern}")
        message(STATUS "  -> Pulling: ${_dep}")
        get_filename_component(_real_path "${_dep}" REALPATH)
        file(INSTALL "${_real_path}" DESTINATION "${CMAKE_INSTALL_PREFIX}")

        # If it was a symlink, also install the symlink itself
        get_filename_component(_dep_name  "${_dep}"       NAME)
        get_filename_component(_real_name "${_real_path}" NAME)
        if(NOT "${_dep_name}" STREQUAL "${_real_name}")
            file(INSTALL "${_dep}" DESTINATION "${CMAKE_INSTALL_PREFIX}")
        endif()
    endif()
endforeach()
