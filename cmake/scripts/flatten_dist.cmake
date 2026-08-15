# flatten_dist.cmake — Flatten the distribution directory at install time.
# Called by packaging.cmake after artifact harvesting.
message(STATUS "[SGRN-PKG] Flattening distribution...")
if(EXISTS "${CMAKE_INSTALL_PREFIX}/bin")
    file(COPY "${CMAKE_INSTALL_PREFIX}/bin/" DESTINATION "${CMAKE_INSTALL_PREFIX}")
    file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/bin")
endif()
file(REMOVE_RECURSE
    "${CMAKE_INSTALL_PREFIX}/lib"
    "${CMAKE_INSTALL_PREFIX}/include"
    "${CMAKE_INSTALL_PREFIX}/share"
)
