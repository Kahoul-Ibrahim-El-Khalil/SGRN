project(sgrn_ml LANGUAGES CXX)

file(GLOB_RECURSE ML_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE ML_HEADERS CONFIGURE_DEPENDS include/sgrn/ml/*.hpp)

sgrn_add_component_library(
    TARGET sgrn_ml
    ALIAS sgrn::ml
    TYPE SHARED
    SOURCES ${ML_SOURCES}
    HEADERS ${ML_HEADERS}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PCH sgrn_pch_third
    ENABLE_ORIGIN_RPATH
    PUBLIC_LIBS
        sgrn::core
        sgrn::utils
        sgrn::fmt
        sgrn::scl   # time_grid_resampler.hpp uses scl/types/DbField in its public API
        sgrn::s7codec
)

sgrn_install_component_library(
    TARGET sgrn_ml
    EXPORT_NAME sgrn_ml
    INCLUDE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/include/sgrn/ml
    INCLUDE_DESTINATION include/sgrn
)

sgrn_target(sgrn_ml)

# =============================================================================
# Unit Tests
# =============================================================================

if(SGRN_BUILD_TESTS)
    foreach(test_name feature_spec_test manifest_test extractor_test engine_test eval_test time_grid_resampler_test columnar_dataset_test)
        sgrn_add_component_executable(
            TARGET ${test_name}
            SOURCES tests/${test_name}.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn_ml
                sgrn::core
                sgrn::utils
                sgrn::fmt
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
        sgrn_target(${test_name})
    endforeach()

    if(NOT WIN32 AND NOT MINGW AND NOT SGRN_CROSS_WIN64)
        sgrn_add_component_executable(
            TARGET pipeline_cli_test
            SOURCES tests/pipeline_cli_test.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn_ml
                sgrn::core
                sgrn::utils
                sgrn::fmt
        )
        target_include_directories(pipeline_cli_test PRIVATE
            ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support
            ${CMAKE_SOURCE_DIR}/.prefix/include/httplib
        )
        add_dependencies(pipeline_cli_test sgrn_dataset sgrn_train sgrn_replay)
        target_compile_definitions(pipeline_cli_test PRIVATE
            SGRN_DATASET_BIN="$<TARGET_FILE:sgrn_dataset>"
            SGRN_TRAIN_BIN="$<TARGET_FILE:sgrn_train>"
            SGRN_REPLAY_BIN="$<TARGET_FILE:sgrn_replay>"
        )
        add_test(NAME pipeline_cli_test COMMAND pipeline_cli_test)
        sgrn_target(pipeline_cli_test)
    endif()
endif()
