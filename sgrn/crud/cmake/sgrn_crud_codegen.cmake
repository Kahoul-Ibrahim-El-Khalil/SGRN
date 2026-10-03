# sgrn/crud/cmake/sgrn_crud_codegen.cmake

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SGRN_CRUD_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

# Custom target for generating CRUD handlers from manifests
add_custom_target(sgrn_crud_generate
    COMMAND ${Python3_EXECUTABLE}
            "${SGRN_CRUD_DIR}/generator/generate_crud.py"
    WORKING_DIRECTORY "${SGRN_CRUD_DIR}"
    COMMENT "Regenerating SGRN CRUD handlers from YAML manifests"
    VERBATIM
)
