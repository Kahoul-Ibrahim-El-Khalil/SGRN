file(GLOB SGRN_UTILS_SOURCES *.cpp )

add_library(sgrn_utils SHARED ${SGRN_UTILS_SOURCES})
target_link_libraries(sgrn_utils PUBLIC 
      sgrn_core
      OpenSSL::SSL 
      OpenSSL::Crypto 
      Poco::Crypto
      Poco::Foundation 
      Poco::Util
      Poco::JWT
      Poco::JSON
      zstd
)
target_include_directories(sgrn_utils PUBLIC ${CMAKE_SOURCE_DIR}/src)
sgrn_install_library(sgrn_utils)
