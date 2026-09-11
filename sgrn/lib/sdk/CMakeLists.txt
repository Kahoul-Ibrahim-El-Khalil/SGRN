project(sgrn_sdk LANGUAGES CXX C)

set(SGRN_SDK_LIB_TYPE SHARED)

set(SOURCES
    src/Domains.cpp
    src/SgrnClient.cpp
)
set(HEADERS
    include/sgrn/sdk/Domains.hpp
    include/sgrn/sdk/SgrnClient.hpp
    include/sgrn/sdk/types.hpp
)

sgrn_add_component_library(
    TARGET sgrn_sdk
    ALIAS sgrn::sdk
    TYPE ${SGRN_SDK_LIB_TYPE}
    SOURCES ${SOURCES}
    HEADERS ${HEADERS}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    ENABLE_ORIGIN_RPATH
    PUBLIC_LIBS
        sgrn::core
        sgrn::utils
        sgrn::fmt
    WIN_LIBS
        ws2_32 crypt32 advapi32
)

sgrn_install_component_library(
    TARGET sgrn_sdk
    EXPORT_NAME sgrn_sdk
    INCLUDE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/include/sgrn/sdk
    INCLUDE_DESTINATION include/sgrn
)

sgrn_package_runtime_dependencies(
    LIBS sgrn_sdk
    COMPONENT sdk
)
