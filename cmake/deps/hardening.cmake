# ── Cross-Compilation Hardening ───────────────────────────────────────────────
if(SGRN_CROSS_ARM64)
    message(STATUS "[SGRN/extern] Hardening ARM64 package discovery for ${SGRN_CONDA_PREFIX}")
    
    # Force standard search variables to the target Conda prefix
    set(ZLIB_ROOT "${SGRN_CONDA_PREFIX}" CACHE PATH "" FORCE)
    set(OPENSSL_ROOT_DIR "${SGRN_CONDA_PREFIX}" CACHE PATH "" FORCE)
    set(CARES_ROOT "${SGRN_CONDA_PREFIX}" CACHE PATH "" FORCE)
    
    # Override potentially broken include directories (like the "/Library/include" issue)
    set(ZLIB_INCLUDE_DIR "${SGRN_CONDA_PREFIX}/include" CACHE PATH "" FORCE)
    set(OPENSSL_INCLUDE_DIR "${SGRN_CONDA_PREFIX}/include" CACHE PATH "" FORCE)
    set(CARES_INCLUDE_DIR "${SGRN_CONDA_PREFIX}/include" CACHE PATH "" FORCE)
    set(C-ARES_INCLUDE_DIRS "${SGRN_CONDA_PREFIX}/include" CACHE PATH "" FORCE)
    
    # Ensure linker can find libraries
    link_directories("${SGRN_CONDA_PREFIX}/lib")

    # Fallback: Manually define OpenSSL, ZLIB, and c-ares targets if find_package fails to create them
    if(NOT TARGET OpenSSL::SSL)
        add_library(OpenSSL::SSL INTERFACE IMPORTED GLOBAL)
        set_target_properties(OpenSSL::SSL PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
        set_target_properties(OpenSSL::SSL PROPERTIES INTERFACE_LINK_LIBRARIES "${SGRN_CONDA_PREFIX}/lib/libssl.so;${SGRN_CONDA_PREFIX}/lib/libcrypto.so")
    endif()
    if(NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto INTERFACE IMPORTED GLOBAL)
        set_target_properties(OpenSSL::Crypto PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
        set_target_properties(OpenSSL::Crypto PROPERTIES INTERFACE_LINK_LIBRARIES "${SGRN_CONDA_PREFIX}/lib/libcrypto.so")
    endif()
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
        set_target_properties(ZLIB::ZLIB PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR}")
        set_target_properties(ZLIB::ZLIB PROPERTIES INTERFACE_LINK_LIBRARIES "${SGRN_CONDA_PREFIX}/lib/libz.so")
    endif()
    if(NOT TARGET c-ares_lib)
        add_library(c-ares_lib INTERFACE IMPORTED GLOBAL)
        set_target_properties(c-ares_lib PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${C-ARES_INCLUDE_DIRS}")
        set_target_properties(c-ares_lib PROPERTIES INTERFACE_LINK_LIBRARIES "${SGRN_CONDA_PREFIX}/lib/libcares.so")
    endif()
endif()

# ── Robust ZLIB/OpenSSL discovery for Windows/ARM64 ────────────────────────────────
if(SGRN_CROSS_WIN64 OR SGRN_CROSS_ARM64)
    find_package(ZLIB QUIET)
    sgrn_repair_target(ZLIB::ZLIB)
    
    find_package(OpenSSL QUIET)
    sgrn_repair_target(OpenSSL::SSL)
    sgrn_repair_target(OpenSSL::Crypto)
endif()
