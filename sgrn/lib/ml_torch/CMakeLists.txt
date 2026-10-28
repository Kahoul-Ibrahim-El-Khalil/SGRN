project(sgrn_ml_torch LANGUAGES CXX)

# Save global CMAKE_CXX_FLAGS to prevent Torch from leaking its flags to subsequent targets
set(_SAVED_CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

option(TORCH_USE_CPU "Build Torch components with default CPU device" OFF)
option(TORCH_USE_GPU "Build Torch components with default GPU (CUDA/ROCm) device" OFF)

if(TORCH_USE_GPU)
    add_compile_definitions(TORCH_USE_GPU)
elseif(TORCH_USE_CPU)
    add_compile_definitions(TORCH_USE_CPU)
endif()

# Locate Torch from the active Conda prefix
find_package(Torch REQUIRED)

# Restore global CMAKE_CXX_FLAGS
set(CMAKE_CXX_FLAGS "${_SAVED_CMAKE_CXX_FLAGS}" CACHE STRING "Flags used by the CXX compiler" FORCE)

file(GLOB_RECURSE ML_TORCH_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE ML_TORCH_HEADERS CONFIGURE_DEPENDS include/sgrn/ml_torch/*.hpp)

sgrn_add_component_library(
    TARGET sgrn_ml_torch
    ALIAS sgrn::ml_torch
    TYPE SHARED
    SOURCES ${ML_TORCH_SOURCES}
    HEADERS ${ML_TORCH_HEADERS}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    PCH sgrn_pch_third
    ENABLE_ORIGIN_RPATH
    PUBLIC_LIBS
        sgrn::ml
        sgrn::core
        sgrn::utils
        sgrn::fmt
        ${TORCH_LIBRARIES}
)

target_include_directories(sgrn_ml_torch SYSTEM PUBLIC
    ${TORCH_INCLUDE_DIRS}
)

if(TORCH_CXX_FLAGS)
    target_compile_options(sgrn_ml_torch PRIVATE ${TORCH_CXX_FLAGS})
endif()

sgrn_install_component_library(
    TARGET sgrn_ml_torch
    EXPORT_NAME sgrn_ml_torch
    INCLUDE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/include/sgrn/ml_torch
    INCLUDE_DESTINATION include/sgrn
)

sgrn_target(sgrn_ml_torch)

# =============================================================================
# Smoke & Unit Tests
# =============================================================================

if(SGRN_BUILD_TESTS)
    foreach(test_name torch_smoke_test mlp_test torch_backend_test trainer_test)
        sgrn_add_component_executable(
            TARGET ${test_name}
            SOURCES tests/${test_name}.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn_ml_torch
                sgrn::ml
                sgrn::core
                sgrn::utils
                sgrn::fmt
                ${TORCH_LIBRARIES}
        )
        target_include_directories(${test_name} SYSTEM PRIVATE
            ${TORCH_INCLUDE_DIRS}
        )
        if(TORCH_CXX_FLAGS)
            target_compile_options(${test_name} PRIVATE ${TORCH_CXX_FLAGS})
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name})
        sgrn_target(${test_name})
    endforeach()
endif()
