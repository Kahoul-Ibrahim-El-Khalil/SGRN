# Read the compile_commands.json
file(READ "${INPUT_FILE}" COMPILE_COMMANDS_CONTENT)

# Remove PCH-related flags
string(REGEX REPLACE " -Winvalid-pch" "" COMPILE_COMMANDS_CONTENT
                     "${COMPILE_COMMANDS_CONTENT}")
string(REGEX REPLACE " -include [^ ]*cmake_pch\\.hxx" ""
                     COMPILE_COMMANDS_CONTENT "${COMPILE_COMMANDS_CONTENT}")
string(REGEX REPLACE " -x c\\+\\+-header" "" COMPILE_COMMANDS_CONTENT
                     "${COMPILE_COMMANDS_CONTENT}")
string(REGEX REPLACE " -o [^ ]*cmake_pch\\.hxx\\.gch" ""
                     COMPILE_COMMANDS_CONTENT "${COMPILE_COMMANDS_CONTENT}")

# Remove the PCH compilation entry entirely (first entry in the JSON array)
string(REGEX REPLACE "\\[[\n\r]*\\{[^}]*cmake_pch\\.hxx\\.cxx[^}]*\\},[\n\r]*"
                     "[" COMPILE_COMMANDS_CONTENT "${COMPILE_COMMANDS_CONTENT}")

# Write the cleaned version
file(WRITE "${OUTPUT_FILE}" "${COMPILE_COMMANDS_CONTENT}")

message(STATUS "Cleaned compile_commands.json written to ${OUTPUT_FILE}")
