# cmake/deps/aws.cmake — AWS SDK manual linkage for Linux
# Included by cmake/deps.cmake for native linux builds.

if(SGRN_SKIP_HOST_DEPS)
    return()
endif()

if(NOT TARGET sgrn::aws)
    add_library(sgrn::aws INTERFACE)
endif()

set(_aws_components
    aws-cpp-sdk-s3
    aws-cpp-sdk-core
    aws-crt-cpp
    aws-c-event-stream
    aws-c-common
    aws-c-io
    aws-c-mqtt
    aws-c-http
    aws-c-auth
    aws-c-cal
    aws-c-compression
    aws-c-sdkutils
    aws-checksums
    s2n
)

# Save the original CMAKE_FIND_LIBRARY_SUFFIXES so we can restore it.
# When CMAKE_FIND_LIBRARY_SUFFIXES is restricted to .a only (e.g. static builds),
# we need to also search .so because the AWS SDK from conda only ships shared
# libraries. We temporarily extend the suffixes for the find_library calls below.
set(_aws_saved_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
if(CMAKE_FIND_LIBRARY_SUFFIXES MATCHES "\\.a")
    # Extend suffixes to include .so so we can find the conda AWS SDK libs.
    # On Linux, shared libraries are the norm for AWS SDK from conda-forge.
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_FIND_LIBRARY_SUFFIXES};.so")
endif()

set(_aws_libs "")
foreach(_comp ${_aws_components})
    # Search in LINUX_PREFIX first (usually /usr or conda prefix for non-static builds)
    find_library(AWS_${_comp}_LIB NAMES ${_comp}
        PATHS "${LINUX_PREFIX}/lib" NO_DEFAULT_PATH)
    # Fall back to SGRN_CONDA_PREFIX (AWS SDK from conda-forge only ships shared libs)
    if(NOT AWS_${_comp}_LIB AND SGRN_CONDA_PREFIX)
        find_library(AWS_${_comp}_LIB NAMES ${_comp}
            PATHS "${SGRN_CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
    endif()
    if(AWS_${_comp}_LIB)
        list(APPEND _aws_libs "${AWS_${_comp}_LIB}")
    endif()
endforeach()

# Restore original suffixes
set(CMAKE_FIND_LIBRARY_SUFFIXES "${_aws_saved_suffixes}")

if(AWS_aws-cpp-sdk-core_LIB AND AWS_aws-cpp-sdk-s3_LIB)
    message(STATUS "[SGRN] AWS SDK found. Linking ${_aws_libs}")
    target_link_libraries(sgrn::aws INTERFACE "-Wl,--no-as-needed")
    foreach(_lib ${_aws_libs})
        target_link_libraries(sgrn::aws INTERFACE "${_lib}")
    endforeach()
    target_link_libraries(sgrn::aws INTERFACE "-Wl,--as-needed")
    target_include_directories(sgrn::aws INTERFACE "${LINUX_PREFIX}/include")
endif()