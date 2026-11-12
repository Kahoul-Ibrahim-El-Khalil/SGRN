project(sgrn_utils LANGUAGES CXX C)

# ── Library type ──────────────────────────────────────────────────────────────
set(sgrn_utils_LIB_TYPE SHARED)

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/compression.cpp
    src/crypto.cpp
    src/crypto/dek.cpp
    src/crypto/key-provider.cpp
    src/crypto/aead.cpp
    src/crypto/stream.cpp
    src/jwt.cpp
    src/xml.cpp
    src/xml_impl.c
    src/encoding.cpp
    src/network.cpp
)
set(HEADERS
    include/sgrn/utils/app.hpp
    include/sgrn/utils/compression.hpp
    include/sgrn/utils/concurrent-queue.hpp
    include/sgrn/utils/crypto.hpp
    include/sgrn/utils/crypto/dek.hpp
    include/sgrn/utils/crypto/key-provider.hpp
    include/sgrn/utils/crypto/aead.hpp
    include/sgrn/utils/crypto/stream.hpp
    include/sgrn/utils/encoding.hpp
    include/sgrn/utils/endianess.hpp
    include/sgrn/utils/env.hpp
    include/sgrn/utils/event-broker.hpp
    include/sgrn/utils/filesystem.hpp
    include/sgrn/utils/glaze.hpp
    include/sgrn/utils/json-dom.hpp
    include/sgrn/utils/json-ops.hpp
    include/sgrn/utils/json-serialization.hpp
    include/sgrn/utils/hashing.hpp
    include/sgrn/utils/jsoncpp.hpp
    include/sgrn/utils/json.hpp
    include/sgrn/utils/jwt.hpp
    include/sgrn/utils/keygen.hpp
    include/sgrn/utils/mime.hpp
    include/sgrn/utils/network.hpp
    include/sgrn/utils/process.hpp
    include/sgrn/utils/progress.hpp
    include/sgrn/utils/strings.hpp
    include/sgrn/utils/tar.hpp
    include/sgrn/utils/threading.hpp
    include/sgrn/utils/time.hpp
    include/sgrn/utils/xml.hpp
)

# Disable PCH for C files which cannot use the CXX PCH
set_source_files_properties(src/xml_impl.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

sgrn_add_component_library(
    TARGET sgrn_utils
    ALIAS sgrn::utils
    TYPE ${sgrn_utils_LIB_TYPE}
    SOURCES ${SOURCES}
    HEADERS ${HEADERS}
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    ENABLE_ORIGIN_RPATH
    PUBLIC_LIBS
        sgrn::core
        sgrn::zlib
        sgrn::zstd
        sgrn::openssl
    WIN_LIBS
        ws2_32 crypt32 advapi32
)

# cpp-httplib (header-only) — headers exposed via extern::httplib target
target_link_libraries(sgrn_utils PUBLIC extern::httplib)

# Embed fmt symbols privately (binary-safe), but propagate headers to consumers
target_link_libraries(sgrn_utils PUBLIC fmt::fmt)
target_include_directories(sgrn_utils INTERFACE $<TARGET_PROPERTY:fmt::fmt,INTERFACE_INCLUDE_DIRECTORIES>)

# Glaze (header-only, C++23 JSON) — propagated to all sgrn_utils consumers so
# that sgrn/utils/glaze.hpp and sgrn::json::* are available without extra links.
target_link_libraries(sgrn_utils PUBLIC extern::glaze)


target_link_libraries(sgrn_utils PRIVATE extern::xml_h)

# jwt remains on jsoncpp; not propagated to Edge consumers
target_link_libraries(sgrn_utils PRIVATE sgrn::jsoncpp)

sgrn_install_component_library(
    TARGET sgrn_utils
    EXPORT_NAME sgrn_utils
    INCLUDE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/include/sgrn/utils
    INCLUDE_DESTINATION include/sgrn
)

sgrn_package_runtime_dependencies(
    LIBS sgrn_utils
    COMPONENT utils
)

# ── Tests ─────────────────────────────────────────────────────────────────────

if(SGRN_BUILD_TESTS)
    sgrn_add_component_executable(
        TARGET glaze_json_test
        SOURCES tests/glaze-json-test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_utils
            fmt::fmt
            extern::glaze
    )
    add_test(NAME glaze_json_test COMMAND glaze_json_test)
    target_compile_features(glaze_json_test PRIVATE cxx_std_23)

    sgrn_add_component_executable(
        TARGET compression_test
        SOURCES tests/compression-test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_utils
            fmt::fmt
            sgrn::zstd
    )
    add_test(NAME compression_test COMMAND compression_test)
    target_compile_features(compression_test PRIVATE cxx_std_23)

    sgrn_add_component_executable(
        TARGET crypto_test
        SOURCES tests/crypto-test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_utils
            fmt::fmt
            sgrn::openssl
    )
    add_test(NAME crypto_test COMMAND crypto_test)
    target_compile_features(crypto_test PRIVATE cxx_std_23)
endif()
