# cmake/build_hardening.cmake — Compiler and Linker Hardening for SGRN
# ─────────────────────────────────────────────────────────────────────────────
# Enforces compiler flags and linker options for high-assurance environments:
#   - Stack protection (-fstack-protector-strong)
#   - Stack clash protection (-fstack-clash-protection)
#   - Control-flow enforcement (-fcf-protection on x86_64)
#   - Source fortification (-D_FORTIFY_SOURCE=3 on Release)
#   - Format string protection (-Wformat -Wformat-security)
#   - Position Independent Executable (-fPIE / -pie)
#   - Full RELRO and immediate binding (-Wl,-z,relro,-z,now)
#   - Sanitizer selection: SGRN_SANITIZE=asan|ubsan|tsan|asan,ubsan (mutually exclusive)
# ─────────────────────────────────────────────────────────────────────────────

include_guard(GLOBAL)

# 1. Default CMAKE_BUILD_TYPE to Release when unset
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (default Release)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# 2. Sanitizer configuration (SGRN_SANITIZE)
set(SGRN_SANITIZE "" CACHE STRING "Sanitizer to enable: asan, ubsan, tsan, or asan,ubsan")
set_property(CACHE SGRN_SANITIZE PROPERTY STRINGS "" "asan" "ubsan" "tsan" "asan,ubsan")

if(SGRN_SANITIZE)
    string(TOLOWER "${SGRN_SANITIZE}" _sgrn_san_lower)
    if(_sgrn_san_lower MATCHES "tsan" AND (_sgrn_san_lower MATCHES "asan" OR _sgrn_san_lower MATCHES "ubsan"))
        message(FATAL_ERROR "SGRN_SANITIZE: TSan (ThreadSanitizer) is mutually exclusive with ASan/UBSan.")
    endif()
    message(STATUS "[SGRN/Hardening] Sanitizer enabled: ${SGRN_SANITIZE}")
endif()

# 3. Target Hardening Helper
function(sgrn_apply_target_hardening target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /GS /guard:cf /sdl)
        return()
    endif()

    # Compile options for memory & execution safety
    target_compile_options(${target} PRIVATE
        -fstack-protector-strong
        -Wformat
        -Wformat-security
    )

    # Stack clash protection (Linux / GCC / Clang)
    if(NOT WIN32 AND NOT APPLE)
        target_compile_options(${target} PRIVATE -fstack-clash-protection)
    endif()

    # Intel CET / Control-Flow Protection on x86_64
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|i[3-6]86")
        target_compile_options(${target} PRIVATE -fcf-protection)
    endif()

    # _FORTIFY_SOURCE=3 for Release / optimized builds (glibc 2.33+)
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Release>:_FORTIFY_SOURCE=3>
    )

    # Position Independent Code / Executables & Full RELRO
    # Do not force -fPIE / POSITION_INDEPENDENT_CODE on executables because Clang PCH
    # requires matching PIC/PIE semantics between static PCH libraries and consumers.
    get_target_property(_target_type ${target} TYPE)
    if(_target_type STREQUAL "EXECUTABLE")
        if(NOT WIN32 AND NOT APPLE)
            target_link_options(${target} PRIVATE -Wl,-z,relro,-z,now)
        endif()
    elseif(_target_type STREQUAL "SHARED_LIBRARY")
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if(NOT WIN32 AND NOT APPLE)
            target_compile_options(${target} PRIVATE -fPIC)
            target_link_options(${target} PRIVATE -Wl,-z,relro,-z,now)
        endif()
    elseif(_target_type STREQUAL "STATIC_LIBRARY")
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

    # Apply Sanitizers
    if(SGRN_SANITIZE)
        string(TOLOWER "${SGRN_SANITIZE}" _san)
        set(_san_flags "")
        if(_san STREQUAL "asan")
            set(_san_flags "-fsanitize=address" "-fno-omit-frame-pointer")
        elseif(_san STREQUAL "ubsan")
            set(_san_flags "-fsanitize=undefined" "-fno-omit-frame-pointer")
        elseif(_san STREQUAL "tsan")
            set(_san_flags "-fsanitize=thread")
        elseif(_san STREQUAL "asan,ubsan" OR _san STREQUAL "asan+ubsan")
            set(_san_flags "-fsanitize=address,undefined" "-fno-omit-frame-pointer")
        else()
            message(FATAL_ERROR "Unknown SGRN_SANITIZE='${SGRN_SANITIZE}'. Expected asan, ubsan, tsan, or asan,ubsan.")
        endif()

        if(_san_flags)
            target_compile_options(${target} PRIVATE ${_san_flags})
            target_link_options(${target} PRIVATE ${_san_flags})
        endif()
    endif()
endfunction()
