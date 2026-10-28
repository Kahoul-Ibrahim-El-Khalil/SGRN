# sgrn_ml_eval — SGRN Model Evaluation & Trace Comparator CLI
sgrn_add_component_executable(
    TARGET sgrn_ml_eval
    SOURCES main.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_ml
        sgrn_utils
        sgrn::core
        sgrn::cxxopts
        fmt::fmt
    WIN_LIBS
        ${WIN_LIBS_LIST}
)

if(SGRN_WITH_TORCH)
    target_link_libraries(sgrn_ml_eval PRIVATE
        sgrn_ml_torch
        ${TORCH_LIBRARIES}
    )
    target_include_directories(sgrn_ml_eval SYSTEM PRIVATE
        ${TORCH_INCLUDE_DIRS}
    )
    if(TORCH_CXX_FLAGS)
        target_compile_options(sgrn_ml_eval PRIVATE ${TORCH_CXX_FLAGS})
    endif()
    target_compile_definitions(sgrn_ml_eval PRIVATE SGRN_HAS_TORCH=1)
endif()

sgrn_target(sgrn_ml_eval)
sgrn_install_component_executable(TARGET sgrn_ml_eval COMPONENT ml)
