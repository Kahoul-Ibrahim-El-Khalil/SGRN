# ── Drogon & Trantor ──────────────────────────────────────────────────────────
# Source is obtained via CPM.cmake (declared in FetchDeps.cmake).
# GIT_SUBMODULES="trantor" in the CPMAddPackage declaration ensures trantor
# is cloned alongside drogon into the same source tree.
set(BUILD_TESTING    OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(BUILD_DOC        OFF CACHE BOOL "" FORCE)

if(SGRN_BUILD_DROGON)
    set(DROGON_FOUND_PRECOMPILED OFF)
    if(NOT SGRN_DEPS_ONLY AND (SGRN_LOCAL_PREFIX OR SGRN_DEPS_PREFIX))
        set(_search_prefix "${SGRN_LOCAL_PREFIX}")
        if(NOT _search_prefix)
            set(_search_prefix "${SGRN_DEPS_PREFIX}")
        endif()

        if(SGRN_CROSS_WIN64 AND SGRN_WIN64_PREFIX)
            # Help pre-compiled config files find OpenSSL
            set(OPENSSL_ROOT_DIR "${SGRN_WIN64_PREFIX}/Library")
            set(OPENSSL_USE_STATIC_LIBS OFF)
            find_package(OpenSSL QUIET)
        endif()

        find_package(Drogon CONFIG QUIET PATHS "${_search_prefix}" NO_DEFAULT_PATH)
        find_package(Trantor CONFIG QUIET PATHS "${_search_prefix}" NO_DEFAULT_PATH)
        if(Drogon_FOUND AND Trantor_FOUND)
            # message(STATUS "[SGRN] Found pre-compiled Drogon and Trantor in ${_search_prefix}")
            set(DROGON_FOUND_PRECOMPILED ON)
            # Create global aliases so dependency resolution still works.
            if(NOT TARGET drogon)
                add_library(drogon INTERFACE IMPORTED GLOBAL)
                target_link_libraries(drogon INTERFACE Drogon::Drogon)
            endif()
            if(NOT TARGET trantor)
                add_library(trantor INTERFACE IMPORTED GLOBAL)
                target_link_libraries(trantor INTERFACE Trantor::Trantor)
            endif()
            if(SGRN_CROSS_WIN64 AND TARGET sgrn::jsoncpp)
                target_link_libraries(Drogon::Drogon INTERFACE sgrn::jsoncpp)
                target_link_libraries(Trantor::Trantor INTERFACE sgrn::jsoncpp)
            endif()
            if(SGRN_CROSS_WIN64 AND TARGET sgrn::fmt)
                target_link_libraries(Drogon::Drogon INTERFACE sgrn::fmt)
                target_link_libraries(Trantor::Trantor INTERFACE sgrn::fmt)
            endif()
        endif()
    endif()

    # Also skip CPM rebuild if extern-config.cmake already registered drogon as IMPORTED
    if(NOT DROGON_FOUND_PRECOMPILED AND NOT TARGET drogon)
        # Force STATIC builds to avoid thread_local dllexport issues on MinGW/Clang
        set(DROGON_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(TRANTOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF) # Local override for this scope
        if(SGRN_CROSS_WIN64 OR SGRN_CROSS_ARM64)
            set(BUILD_CTL OFF CACHE BOOL "" FORCE)

            # The win64 non-backend build does not need Drogon DB backends.
            # Force all cache variables involved in config.h generation off so
            # stale CMake cache entries cannot leak SQLite/PostgreSQL support
            # macros into a trantor-only consumer build.
            if(SGRN_WIN64_MSYS2_ROOT)
                message(STATUS "[extern] MSYS2 ucrt64 prefix for Drogon deps: ${SGRN_WIN64_MSYS2_ROOT}")
                set(_m_inc "${SGRN_WIN64_MSYS2_ROOT}/include")
                set(_m_lib "${SGRN_WIN64_MSYS2_ROOT}/lib")

                set(OPENSSL_USE_STATIC_LIBS  OFF CACHE BOOL "" FORCE)
                set(OPENSSL_MSVC_STATIC_RT   OFF CACHE BOOL "" FORCE)
                set(OPENSSL_INCLUDE_DIR      "${_m_inc}"                 CACHE PATH     "" FORCE)
                set(OPENSSL_CRYPTO_LIBRARY   "${_m_lib}/libcrypto.dll.a" CACHE FILEPATH "" FORCE)
                set(OPENSSL_SSL_LIBRARY      "${_m_lib}/libssl.dll.a"    CACHE FILEPATH "" FORCE)
                set(OPENSSL_FOUND            TRUE                         CACHE BOOL     "" FORCE)

                set(ZLIB_INCLUDE_DIR         "${_m_inc}"                 CACHE PATH     "" FORCE)
                set(ZLIB_LIBRARY             "${_m_lib}/libz.dll.a"      CACHE FILEPATH "" FORCE)
                set(ZLIB_FOUND               TRUE                         CACHE BOOL     "" FORCE)

                set(CARES_INCLUDE_DIR        "${_m_inc}"                 CACHE PATH     "" FORCE)
                set(CARES_LIBRARY            "${_m_lib}/libcares.dll.a"  CACHE FILEPATH "" FORCE)
                set(CARES_FOUND              TRUE                         CACHE BOOL     "" FORCE)

                set(JSONCPP_INCLUDE_DIRS     "${_m_inc}"                 CACHE PATH     "" FORCE)
                set(JSONCPP_LIBRARIES        "${_m_lib}/libjsoncpp.dll.a" CACHE FILEPATH "" FORCE)
                set(JSONCPP_FOUND            TRUE                         CACHE BOOL     "" FORCE)
            else()
                message(WARNING "[extern] SGRN_WIN64_MSYS2_ROOT not set — MSVC/MinGW ABI mismatch likely!")
                if(SGRN_WIN64_PREFIX)
                    if(NOT SGRN_WIN64_OPENSSL_ROOT)
                        set(SGRN_WIN64_OPENSSL_ROOT "${SGRN_WIN64_PREFIX}")
                    endif()
                    if(NOT SGRN_WIN64_ZLIB_ROOT)
                        set(SGRN_WIN64_ZLIB_ROOT    "${SGRN_WIN64_PREFIX}")
                    endif()
                    if(NOT SGRN_WIN64_CARES_ROOT)
                        set(SGRN_WIN64_CARES_ROOT   "${SGRN_WIN64_PREFIX}")
                    endif()
                endif()
                set(OPENSSL_USE_STATIC_LIBS OFF CACHE BOOL "" FORCE)
                set(OPENSSL_MSVC_STATIC_RT  OFF CACHE BOOL "" FORCE)
                set(OPENSSL_INCLUDE_DIR    "${SGRN_WIN64_OPENSSL_ROOT}/Library/include"          CACHE PATH     "" FORCE)
                set(OPENSSL_CRYPTO_LIBRARY "${SGRN_WIN64_OPENSSL_ROOT}/Library/lib/libcrypto.lib" CACHE FILEPATH "" FORCE)
                set(OPENSSL_SSL_LIBRARY    "${SGRN_WIN64_OPENSSL_ROOT}/Library/lib/libssl.lib"    CACHE FILEPATH "" FORCE)
                set(OPENSSL_FOUND          TRUE                                                  CACHE BOOL     "" FORCE)
                set(OpenSSL_FOUND          TRUE                                                  CACHE BOOL     "" FORCE)
                
                set(ZLIB_USE_STATIC_LIBS   OFF CACHE BOOL "" FORCE)
                set(ZLIB_LIBRARY           "${SGRN_WIN64_ZLIB_ROOT}/Library/lib/zlib.lib"        CACHE FILEPATH "" FORCE)
                set(ZLIB_INCLUDE_DIR       "${SGRN_WIN64_ZLIB_ROOT}/Library/include"             CACHE PATH     "" FORCE)
                set(ZLIB_FOUND             TRUE                                                  CACHE BOOL     "" FORCE)
                
                set(CARES_LIBRARY          "${SGRN_WIN64_CARES_ROOT}/Library/lib/cares.lib"      CACHE FILEPATH "" FORCE)
                set(CARES_INCLUDE_DIR      "${SGRN_WIN64_CARES_ROOT}/Library/include"            CACHE PATH     "" FORCE)
                set(CARES_FOUND            TRUE                                                  CACHE BOOL     "" FORCE)
            endif()

            add_definitions(-DJSONCPP_DLL)
            set(LIBPQ_BATCH_MODE       OFF CACHE BOOL "" FORCE)
            set(LIBPQ_SUPPORTS_BATCH_MODE OFF CACHE BOOL "" FORCE)
        else()
            set(BUILD_CTL OFF CACHE BOOL "" FORCE)
        endif()

        if(NOT TARGET Jsoncpp_lib AND TARGET sgrn::jsoncpp)
            add_library(Jsoncpp_lib INTERFACE IMPORTED GLOBAL)
            set_target_properties(Jsoncpp_lib PROPERTIES
                INTERFACE_LINK_LIBRARIES sgrn::jsoncpp
            )
        endif()

        # Prevent drogon from finding jsoncpp in the Conda environment which clashes with
        # the vendored targets. We create a dummy config that does nothing because
        # the jsoncpp targets (jsoncpp_lib, jsoncpp_static) are already defined by our tree.
        if(TARGET sgrn::jsoncpp)
            set(jsoncpp_DIR "${CMAKE_CURRENT_BINARY_DIR}/dummy_jsoncpp" CACHE PATH "" FORCE)
            file(MAKE_DIRECTORY "${jsoncpp_DIR}")
            file(WRITE "${jsoncpp_DIR}/jsoncppConfig.cmake" "set(jsoncpp_FOUND TRUE)\n")
        endif()

        sgrn_fetch_source(drogon)

        add_subdirectory(
            "${drogon_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/drogon"
            EXCLUDE_FROM_ALL
        )

        if(NOT WIN32 AND TARGET drogon AND DEFINED ENV{CONDA_PREFIX})
             # Force transitive dependency search for drogon targets on Linux
             target_link_options(drogon PUBLIC "-Wl,-rpath-link,$ENV{CONDA_PREFIX}/lib")
             if(TARGET _drogon_ctl)
                 target_link_options(_drogon_ctl PRIVATE "-Wl,-rpath-link,$ENV{CONDA_PREFIX}/lib")
             endif()
        endif()

        if(SGRN_CROSS_WIN64 AND TARGET drogon AND TARGET sgrn::jsoncpp)
            target_link_libraries(drogon PUBLIC sgrn::jsoncpp)
        endif()
        if(SGRN_CROSS_WIN64 AND TARGET drogon AND TARGET sgrn::fmt)
            target_link_libraries(drogon PUBLIC sgrn::fmt)
        endif()
    else()
        if(NOT TARGET Jsoncpp_lib AND TARGET sgrn::jsoncpp)
            add_library(Jsoncpp_lib INTERFACE IMPORTED GLOBAL)
            target_link_libraries(Jsoncpp_lib INTERFACE sgrn::jsoncpp)
        endif()
    endif()
else()
    # Win64 non-backend builds only need Trantor. Avoid building Drogon
    # entirely so the cross-build does not pull in unused ORM/db features.
    set(TRANTOR_FOUND_PRECOMPILED OFF)
    if(NOT SGRN_DEPS_ONLY AND (SGRN_LOCAL_PREFIX OR SGRN_DEPS_PREFIX))
        set(_search_prefix "${SGRN_LOCAL_PREFIX}")
        if(NOT _search_prefix)
            set(_search_prefix "${SGRN_DEPS_PREFIX}")
        endif()

        find_package(Trantor CONFIG QUIET PATHS "${_search_prefix}")
        if(Trantor_FOUND)
            set(TRANTOR_FOUND_PRECOMPILED ON)
            if(NOT TARGET trantor)
                add_library(trantor INTERFACE IMPORTED GLOBAL)
                target_link_libraries(trantor INTERFACE Trantor::Trantor)
            endif()
        endif()
    endif()

    # Also skip CPM rebuild if extern-config.cmake already registered trantor as IMPORTED
    if(NOT TRANTOR_FOUND_PRECOMPILED AND NOT TARGET trantor)
        sgrn_fetch_source(drogon)
        add_subdirectory(
            "${drogon_SOURCE_DIR}/trantor"
            "${CMAKE_CURRENT_BINARY_DIR}/trantor"
            EXCLUDE_FROM_ALL
        )
    endif()
endif() # Closes else() of SGRN_BUILD_DROGON
