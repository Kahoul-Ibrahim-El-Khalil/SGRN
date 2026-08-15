# cmake/debug_flags.cmake - Debug option definitions
set(DEBUG_OPTIONS
    DEBUG_QUERY_API
    DEBUG_AUTH_API
    DEBUG_UPLOAD_API
    DEBUG_ADMIN_API
    DEBUG_DOWNLOAD_API
    DEBUG_FILEMETADATA_API
    DEBUG_SESSION_INFO
    DEBUG_GPAO_API
    DEBUG_ORM
    DEBUG_POSTGRES_REFLECTION
    FILE_NAMES_AND_LINES
    WITH_TIME_SIGNATURE
    DEBUG_RESPONSES
    DEBUG_STORAGE
    DEBUG_POSTGREST
    DEBUG_HTTP_REQUESTS
)

# Create options for each debug flag
foreach(flag ${DEBUG_OPTIONS})
    option(${flag} "Enable ${flag}" OFF)
endforeach()

# In RELEASE mode, force all debug flags OFF
if(RELEASE)
    message(STATUS "RELEASE mode: all debug flags forcibly disabled")
    foreach(flag ${DEBUG_OPTIONS})
        set(${flag} OFF CACHE BOOL "Disabled in RELEASE" FORCE)
    endforeach()
endif()

# Apply debug definitions to sgrn_datastore_core
# Called after sgrn_datastore_core is defined
function(sgrn_apply_debug_definitions target)
    foreach(flag ${DEBUG_OPTIONS})
        if(${flag})
            target_compile_definitions(${target} INTERFACE ${flag})
        endif()
    endforeach()
endfunction()
