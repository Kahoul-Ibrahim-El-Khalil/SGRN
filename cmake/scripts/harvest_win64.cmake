# harvest_win64.cmake — Win64 DLL dependency puller
message(STATUS "[SGRN-PKG/win64] Harvesting dependencies...")

# Unquoted expansion drops empty variables, so no stray ";" elements
set(_search_dirs ${SGRN_HARVEST_SEARCH_DIRS})
set(_to_process  ${SGRN_HARVEST_EXECUTABLES} ${SGRN_HARVEST_LIBRARIES})
list(FILTER _to_process EXCLUDE REGEX "^$")
list(REMOVE_DUPLICATES _to_process)
set(_processed "")
set(_installed "")

if(NOT _to_process)
    message(FATAL_ERROR
        "[SGRN-PKG/win64] Nothing to harvest: SGRN_HARVEST_EXECUTABLES and "
        "SGRN_HARVEST_LIBRARIES are both empty. Pass them via install(CODE ...) "
        "before including this script.")
endif()
if(NOT SGRN_OBJDUMP)
    message(FATAL_ERROR "[SGRN-PKG/win64] SGRN_OBJDUMP is not set")
endif()

set(_system_dll_regex
    "^(kernel32|user32|gdi32|winspool|comdlg32|advapi32|shell32|ole32|oleaut32|uuid|odbc32|odbccp32|ws2_32|msvcrt|wsock32|rpcrt4|dnsapi|iphlpapi|secur32|shlwapi|winmm|userenv|bcrypt|crypt32|imm32|comctl32|mpr|netapi32|version|wldap32|winhttp)\\.dll$")

while(_to_process)
    list(GET _to_process 0 _current)
    list(REMOVE_AT _to_process 0)

    list(FIND _processed "${_current}" _seen_idx)
    if(NOT _seen_idx EQUAL -1 OR NOT EXISTS "${_current}")
        continue()
    endif()
    list(APPEND _processed "${_current}")

    execute_process(
        COMMAND "${SGRN_OBJDUMP}" -p "${_current}"
        OUTPUT_VARIABLE _dump
        RESULT_VARIABLE _res
        ERROR_QUIET)
    if(NOT _res EQUAL 0)
        continue()
    endif()

    string(REGEX MATCHALL "DLL Name: [A-Za-z0-9._+-]+" _dll_lines "${_dump}")
    foreach(_line ${_dll_lines})
        string(REPLACE "DLL Name: " "" _dll_name "${_line}")
        string(TOLOWER "${_dll_name}" _dll_name_lc)
        if(_dll_name_lc MATCHES "${_system_dll_regex}")
            continue()
        endif()
        # api-ms-win-* and ext-ms-* are OS forwarders, never shipped
        if(_dll_name_lc MATCHES "^(api-ms-win-|ext-ms-)")
            continue()
        endif()

        set(_found "")
        foreach(_dir ${_search_dirs})
            if(EXISTS "${_dir}/${_dll_name}")
                set(_found "${_dir}/${_dll_name}")
                break()
            endif()
            if(EXISTS "${_dir}/${_dll_name_lc}")
                set(_found "${_dir}/${_dll_name_lc}")
                break()
            endif()
        endforeach()

        if(NOT _found)
            message(STATUS "  -> Not found in search dirs: ${_dll_name}")
            continue()
        endif()

        get_filename_component(_found_name "${_found}" NAME)
        list(FIND _installed "${_found_name}" _inst_idx)
        if(_inst_idx EQUAL -1)
            message(STATUS "  -> Harvesting: ${_found_name}")
            file(INSTALL "${_found}" DESTINATION "${CMAKE_INSTALL_PREFIX}")
            list(APPEND _installed "${_found_name}")
            list(APPEND _to_process "${_found}")
        endif()
    endforeach()
endwhile()

message(STATUS "[SGRN-PKG/win64] Harvested ${_installed}")
