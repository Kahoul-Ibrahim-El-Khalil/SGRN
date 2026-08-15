# cmake/core.cmake — Standard compile flags.
# ─────────────────────────────────────────────────────────────────────────────
# Included by: sgrn/gateway/CMakeLists.txt
#
# Provides sgrn_target(<target>) which applies:
#   - C++23 language standard
#   - Wall/Wextra diagnostic flags
#   - Wno-unused-parameter (open62541 C API callbacks have many unused params)
# ─────────────────────────────────────────────────────────────────────────────
function(sgrn_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_23)
    target_compile_options(${target_name} PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
    )
endfunction()
