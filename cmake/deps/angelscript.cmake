# cmake/deps/angelscript.cmake
# Exposes: extern::angelscript, extern::angelscript_addons

if(TARGET angelscript)
    return()
endif()

sgrn_fetch_source(angelscript)

# CPM with DOWNLOAD_ONLY YES sets angelscript_SOURCE_DIR to the repo root.
# The actual CMakeLists.txt lives inside sdk/angelscript/projects/cmake.
# The true repo root is also in CPM_PACKAGE_angelscript_SOURCE_DIR.
set(_AS_ROOT ${CPM_PACKAGE_angelscript_SOURCE_DIR})
set(angelscript_SOURCE_DIR "${_AS_ROOT}/sdk/angelscript/projects/cmake")

add_subdirectory(
    "${angelscript_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/angelscript"
    EXCLUDE_FROM_ALL
)

if(TARGET angelscript)
    if(NOT TARGET Angelscript::angelscript)
        add_library(Angelscript::angelscript ALIAS angelscript)
    endif()
    target_include_directories(angelscript SYSTEM PUBLIC
        $<BUILD_INTERFACE:${_AS_ROOT}/sdk/angelscript/include>
        $<INSTALL_INTERFACE:include>
    )
    set_target_properties(angelscript PROPERTIES POSITION_INDEPENDENT_CODE ON)

    set(_AS_ADDON ${_AS_ROOT}/sdk/add_on)
    add_library(angelscript_addons STATIC
        ${_AS_ADDON}/scriptstdstring/scriptstdstring.cpp
        ${_AS_ADDON}/scriptarray/scriptarray.cpp
        ${_AS_ADDON}/scripthelper/scripthelper.cpp
        ${_AS_ADDON}/scriptbuilder/scriptbuilder.cpp
        ${_AS_ADDON}/scriptdictionary/scriptdictionary.cpp
    )
    target_include_directories(angelscript_addons SYSTEM PUBLIC
        $<BUILD_INTERFACE:${_AS_ADDON}>
        $<BUILD_INTERFACE:${_AS_ROOT}/sdk/angelscript/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(angelscript_addons PUBLIC Angelscript::angelscript)
    set_target_properties(angelscript_addons PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(extern::angelscript_addons ALIAS angelscript_addons)
    add_library(extern::angelscript ALIAS angelscript)
    sgrn_add_to_install(angelscript_addons)
endif()