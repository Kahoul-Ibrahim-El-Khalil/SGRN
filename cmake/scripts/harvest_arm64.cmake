# harvest_arm64.cmake — ARM64 cross-compile dependency puller
message(STATUS "[SGRN-PKG/arm64] Starting ARM64 dependency harvesting (Full Runtime Mode)...")
if(POLICY CMP0057)
    cmake_policy(SET CMP0057 NEW) # enable IN_LIST in if()
endif()

set(_readelf "${SGRN_READELF}")
if(_readelf STREQUAL "" OR _readelf MATCHES "-NOTFOUND$")
    message(FATAL_ERROR "[SGRN-PKG/arm64] readelf not found")
endif()
if(NOT EXISTS "${_readelf}")
    message(FATAL_ERROR "[SGRN-PKG/arm64] readelf path does not exist: ${_readelf}")
endif()

set(_search_dirs "${SGRN_HARVEST_SEARCH_DIRS}")
set(_to_process "${SGRN_HARVEST_EXECUTABLES};${SGRN_HARVEST_LIBRARIES}")
set(_processed "")
set(_installed "")

while(_to_process)
    list(GET _to_process 0 _current)
    list(REMOVE_AT _to_process 0)
    
    if("${_current}" IN_LIST _processed OR NOT EXISTS "${_current}")
        continue()
    endif()
    list(APPEND _processed "${_current}")

    execute_process(
        COMMAND "${_readelf}" -d "${_current}"
        OUTPUT_VARIABLE _dump
        RESULT_VARIABLE _res
    )

    if(NOT _res EQUAL 0)
        continue()
    endif()

    string(REGEX MATCHALL "NEEDED" _has_needed "${_dump}")
    if(NOT _has_needed)
        continue()
    endif()

    string(REGEX MATCHALL "\\[([^]]+)\\]" _matches "${_dump}")
    
    foreach(_match ${_matches})
        string(REGEX REPLACE "\\[|\\]" "" _lib_name "${_match}")
        if(NOT _lib_name MATCHES "\\.so")
            continue()
        endif()
        
        set(_found "NOTFOUND")
        foreach(_dir ${_search_dirs})
            if(EXISTS "${_dir}/${_lib_name}")
                if(_lib_name MATCHES "^(libc|libm|librt|libdl|libpthread|libutil|libresolv|libnsl|libcrypt|ld-linux-aarch64)\\.so")
                    continue()
                endif()
                set(_found "${_dir}/${_lib_name}")
                break()
            endif()
        endforeach()

        if(_found)
            get_filename_component(_found_name "${_found}" NAME)
            if(NOT "${_found_name}" IN_LIST _installed)
                message(STATUS "  -> Harvesting: ${_found_name}")
                
                get_filename_component(_real_path "${_found}" REALPATH)
                get_filename_component(_real_name "${_real_path}" NAME)
                
                file(INSTALL "${_real_path}" DESTINATION "${CMAKE_INSTALL_PREFIX}")
                list(APPEND _installed "${_real_name}")
                list(APPEND _to_process "${_real_path}")
                
                if(NOT "${_found_name}" STREQUAL "${_real_name}")
                    file(INSTALL "${_found}" DESTINATION "${CMAKE_INSTALL_PREFIX}")
                    list(APPEND _installed "${_found_name}")
                endif()
                
                get_filename_component(_dep_dir "${_found}" DIRECTORY)
                string(REGEX REPLACE "\\.so.*" ".so*" _glob "${_found_name}")
                file(GLOB _aliases "${_dep_dir}/${_glob}")
                foreach(_alias ${_aliases})
                    get_filename_component(_aname "${_alias}" NAME)
                    if(NOT "${_aname}" IN_LIST _installed)
                        file(INSTALL "${_alias}" DESTINATION "${CMAKE_INSTALL_PREFIX}")
                        list(APPEND _installed "${_aname}")
                    endif()
                endforeach()
            endif()
        endif()
    endforeach()
endwhile()
