# cmake/deps/jsoncpp.cmake
# Exposes: jsoncpp_static / jsoncpp_lib + extern::jsoncpp_lib

if(TARGET jsoncpp_static OR TARGET jsoncpp_lib)
    return()
endif()

sgrn_fetch_source(jsoncpp)
set(JSONCPP_WITH_TESTS            OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_CMAKE_PACKAGE    OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_EXAMPLE          OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS             OFF CACHE BOOL "" FORCE)

add_subdirectory("${jsoncpp_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/jsoncpp"
    EXCLUDE_FROM_ALL
)

foreach(_jt IN ITEMS jsoncpp_static jsoncpp_lib)
    if(TARGET ${_jt})
        if(NOT TARGET extern::jsoncpp_lib)
            add_library(extern::jsoncpp_lib ALIAS ${_jt})
        endif()
        break()
    endif()
endforeach()
