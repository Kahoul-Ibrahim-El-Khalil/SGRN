project(sgrn_ml_bridge LANGUAGES CXX)

# sgrn_ml_bridge — Digital-Twin inference bridge.
#
# Bridge layer between the ML runtime (sgrn::ml) and the gateway digital twin
# (sgrn_gateway_twin): TwinBridge extracts PLC variables into model inputs and
# writes predictions back into twin memory. It lives in its own component so
# neither side needs to depend on the other (sgrn_ml stays gateway-free;
# sgrn_gateway_twin stays ML-free). Consumers bind PlcState at app/wiring
# time; per sgrn/lib/README.md "Adding a new library".

file(GLOB_RECURSE ML_BRIDGE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE ML_BRIDGE_HEADERS CONFIGURE_DEPENDS include/sgrn/ml_bridge/*.hpp)

sgrn_add_component_library(
    TARGET sgrn_ml_bridge
    ALIAS sgrn::ml_bridge
    TYPE SHARED
    SOURCES ${ML_BRIDGE_SOURCES}
    HEADERS ${ML_BRIDGE_HEADERS}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PCH sgrn_pch_third
    ENABLE_ORIGIN_RPATH
    PUBLIC_LIBS
        sgrn::ml
        sgrn::core
        sgrn::utils
        sgrn::fmt
        sgrn_gateway_twin
        sgrn::s7codec
)

sgrn_install_component_library(
    TARGET sgrn_ml_bridge
    EXPORT_NAME sgrn_ml_bridge
    INCLUDE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/include/sgrn/ml_bridge
    INCLUDE_DESTINATION include/sgrn
)

sgrn_target(sgrn_ml_bridge)

# =============================================================================
# Unit Tests
# =============================================================================

if(SGRN_BUILD_TESTS)
    foreach(test_name twin_bridge_test pipeline_e2e_test)
        sgrn_add_component_executable(
            TARGET ${test_name}
            SOURCES tests/${test_name}.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn_ml_bridge
                sgrn::core
                sgrn::utils
                sgrn::fmt
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
        sgrn_target(${test_name})
    endforeach()
endif()
