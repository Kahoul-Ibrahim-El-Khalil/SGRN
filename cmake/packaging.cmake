# cmake/packaging.cmake — Runtime Dependency Harvest & Distribution Packaging
# ─────────────────────────────────────────────────────────────────────────────
# Included by: CMakeLists.txt (root)
# Used in   : every sgrn/<component>/CMakeLists.txt via sgrn_package_runtime_dependencies()
#
# Provides sgrn_package_runtime_dependencies(APPS … LIBS … COMPONENT …) which:
#   1. Installs the named executables and shared libraries flat into the
#      dist/<platform>.<component>/ directory (no subdirs).
#   2. Sets $ORIGIN RPATH on all installed binaries so they find sibling .so files
#      without LD_LIBRARY_PATH (Linux/ARM64 only).
#   3. Discovers all transitive .so / .dll runtime dependencies and copies them
#      alongside the binaries using platform-specific harvester scripts:
#        cmake/scripts/harvest_linux.cmake  — ldd-based scan
#        cmake/scripts/harvest_arm64.cmake  — readelf-based scan (cross)
#        cmake/scripts/harvest_win64.cmake  — objdump-based scan (cross)
#   4. Runs cmake/scripts/flatten_dist.cmake to flatten nested install paths.
#
# Static builds (SGRN_BUILD_STATIC=ON, non-Windows) skip steps 3-4 entirely
# because all code is baked into the binary — there are no runtime .so deps.
#
# Output: .dist/<platform>-<build-type>/<component>/
# ─────────────────────────────────────────────────────────────────────────────

include_guard(GLOBAL)

function(sgrn_package_runtime_dependencies)
    set(options "")
    set(oneValueArgs COMPONENT)
    set(multiValueArgs APPS LIBS EXTERNAL_DEPS)
    cmake_parse_arguments(PKG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT PKG_COMPONENT)
        set(PKG_COMPONENT "unspecified")
    endif()

    # 1. Target Tracking (Flat Install)
    # --------------------------------------------------------------------------
    set(_local_executables "")
    foreach(target ${PKG_APPS})
        if(TARGET ${target})
            install(TARGETS ${target} RUNTIME DESTINATION . COMPONENT ${PKG_COMPONENT})
            list(APPEND _local_executables $<TARGET_FILE:${target}>)
            if(UNIX AND NOT APPLE AND NOT SGRN_BUILD_STATIC)
                set_target_properties(${target} PROPERTIES
                    BUILD_WITH_INSTALL_RPATH TRUE
                    SKIP_BUILD_RPATH TRUE
                    INSTALL_RPATH "$ORIGIN"
                    INSTALL_RPATH_USE_LINK_PATH FALSE
                )
            endif()
        endif()
    endforeach()

    # Static builds: everything is baked into the binary — skip .so tracking and harvest.
    # Note: On Windows (and Conda), many dependencies are only available as DLLs,
    # so we still need to harvest runtime dependencies even for "static" builds.
    if(SGRN_BUILD_STATIC AND NOT WIN32 AND NOT SGRN_CROSS_WIN64)
        message(STATUS "[SGRN/packaging] Static build — skipping runtime dependency harvest for '${PKG_COMPONENT}'")
        return()
    endif()

    set(_local_libraries "")
    foreach(target ${PKG_LIBS})
        if(TARGET ${target})
            get_target_property(_t_type ${target} TYPE)
            if(NOT _t_type STREQUAL "STATIC_LIBRARY" AND NOT _t_type STREQUAL "INTERFACE_LIBRARY" AND NOT _t_type STREQUAL "OBJECT_LIBRARY")
                install(TARGETS ${target} 
                    RUNTIME DESTINATION . COMPONENT ${PKG_COMPONENT}
                    LIBRARY DESTINATION . COMPONENT ${PKG_COMPONENT}
                )
                list(APPEND _local_libraries $<TARGET_FILE:${target}>)
            endif()
            if(UNIX AND NOT APPLE)
                set_target_properties(${target} PROPERTIES
                    BUILD_WITH_INSTALL_RPATH TRUE
                    SKIP_BUILD_RPATH TRUE
                    INSTALL_RPATH "$ORIGIN"
                    INSTALL_RPATH_USE_LINK_PATH FALSE
                )
            endif()
        endif()
    endforeach()

    # 2. Dependency Discovery Directories
    # --------------------------------------------------------------------------
    set(_search_dirs "")
    list(APPEND _search_dirs 
        "${CMAKE_BINARY_DIR}/sgrn/utils" 
        "${CMAKE_BINARY_DIR}/sgrn/sdk"
        "${CMAKE_BINARY_DIR}/sgrn/s7"
        "${CMAKE_BINARY_DIR}/bin"
        "${CMAKE_BINARY_DIR}/extern/jsoncpp/lib"
        "${CMAKE_BINARY_DIR}/extern/snap7"
        "${CMAKE_BINARY_DIR}/extern/open62541"
        "${CMAKE_BINARY_DIR}/_deps/fmt-build"
    )

    list(APPEND _search_dirs
        "${SGRN_LOCAL_PREFIX}/lib"
        "${SGRN_LOCAL_PREFIX}/bin"
        "${SGRN_LOCAL_PREFIX}/lib/aarch64-linux-gnu"
    )

    if(SGRN_CROSS_ARM64)
        set(_target_root "${SGRN_CONDA_PREFIX}")
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-sysroot OUTPUT_VARIABLE _sysroot OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_sysroot)
            file(TO_CMAKE_PATH "${_sysroot}" _sysroot)
            list(APPEND _search_dirs "${_sysroot}/lib" "${_sysroot}/lib64" "${_sysroot}/usr/lib" "${_sysroot}/usr/lib/aarch64-linux-gnu")
        endif()
    elseif(SGRN_CROSS_WIN64)
        set(_target_root "${SGRN_WIN64_PREFIX}")
    elseif(DEFINED ENV{CONDA_PREFIX})
        set(_target_root "$ENV{CONDA_PREFIX}")
    endif()

    if(_target_root)
        list(APPEND _search_dirs "${_target_root}/bin" "${_target_root}/Library/bin" "${_target_root}/lib" "${_target_root}/lib/aarch64-linux-gnu" "${_target_root}/mingw-w64/bin")
    endif()

    if(SGRN_CROSS_WIN64)
        set(_host_conda "$ENV{CONDA_PREFIX}")
        if(_host_conda)
            list(APPEND _search_dirs "${_host_conda}/x86_64-w64-mingw32/lib" "${_host_conda}/x86_64-w64-mingw32/bin")
            file(GLOB _gcc_lib_dirs "${_host_conda}/lib/gcc/x86_64-w64-mingw32/*")
            if(_gcc_lib_dirs)
                list(GET _gcc_lib_dirs 0 _gcc_latest)
                list(APPEND _search_dirs "${_gcc_latest}")
            endif()
        endif()
    endif()

    # 3. Inject variables into install-time scope
    # --------------------------------------------------------------------------
    install(CODE "
        set(SGRN_HARVEST_EXECUTABLES \"${_local_executables}\")
        set(SGRN_HARVEST_LIBRARIES   \"${_local_libraries}\")
        set(SGRN_HARVEST_SEARCH_DIRS \"${_search_dirs}\")
    " COMPONENT ${PKG_COMPONENT})

    # 4. Invoke Harvester Scripts
    # --------------------------------------------------------------------------
    if(WIN32 OR SGRN_CROSS_WIN64)
        find_program(SGRN_OBJDUMP NAMES x86_64-w64-mingw32-objdump objdump)
        install(CODE "set(SGRN_OBJDUMP \"${SGRN_OBJDUMP}\")" COMPONENT ${PKG_COMPONENT})
        install(SCRIPT ${CMAKE_SOURCE_DIR}/cmake/scripts/harvest_win64.cmake COMPONENT ${PKG_COMPONENT})
    elseif(SGRN_CROSS_ARM64)
        if(CMAKE_READELF)
            set(SGRN_READELF "${CMAKE_READELF}")
        else()
            find_program(SGRN_READELF NAMES aarch64-conda-linux-gnu-readelf llvm-readelf readelf NO_CMAKE_FIND_ROOT_PATH)
        endif()
        install(CODE "set(SGRN_READELF \"${SGRN_READELF}\")" COMPONENT ${PKG_COMPONENT})
        install(SCRIPT ${CMAKE_SOURCE_DIR}/cmake/scripts/harvest_arm64.cmake COMPONENT ${PKG_COMPONENT})
    else()
        install(SCRIPT ${CMAKE_SOURCE_DIR}/cmake/scripts/harvest_linux.cmake COMPONENT ${PKG_COMPONENT})
    endif()

    # 5. Final Cleanup & Flattening
    install(SCRIPT ${CMAKE_SOURCE_DIR}/cmake/scripts/flatten_dist.cmake COMPONENT ${PKG_COMPONENT})
endfunction()

