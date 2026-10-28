# sgrn_train — In-tree PyTorch model trainer CLI
sgrn_add_component_executable(
    TARGET sgrn_train
    SOURCES main.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_ml_torch
        sgrn_ml
        sgrn_utils
        sgrn::core
        sgrn::cxxopts
        fmt::fmt
        ${TORCH_LIBRARIES}
    WIN_LIBS
        ${WIN_LIBS_LIST}
)

target_include_directories(sgrn_train SYSTEM PRIVATE
    ${TORCH_INCLUDE_DIRS}
)

if(TORCH_CXX_FLAGS)
    target_compile_options(sgrn_train PRIVATE ${TORCH_CXX_FLAGS})
endif()

sgrn_target(sgrn_train)
sgrn_install_component_executable(TARGET sgrn_train COMPONENT ml)
