# ── SGRN Linker Rules Override ───────────────────────────────────────────────
# This file is loaded AFTER CMake's internal platform files.
# It provides the "final word" on how link commands are constructed.


# Only wipe standard runtime libs when using an MSVC-style frontend (clang-cl/MSVC),
# where CMake tends to inject -defaultlib/-nodefaultlib noise.
# For MinGW/gnu-style Clang (our win64 cross toolchain), we must keep the C/C++
# runtime libraries to avoid unresolved libstdc++ symbols at link time.
if(MSVC OR (CMAKE_C_COMPILER MATCHES "clang-cl") OR (CMAKE_CXX_COMPILER MATCHES "clang-cl"))
    set(CMAKE_C_STANDARD_LIBRARIES   "")
    set(CMAKE_CXX_STANDARD_LIBRARIES "")
endif()

# 🚫 Hard-Reset Flag Variables (Strip poisoned internal flags)
# These were populated by Windows-Clang.cmake before this file was loaded.
set(CMAKE_EXE_LINKER_FLAGS    "" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_C_LINK_FLAGS        "" CACHE STRING "" FORCE)
set(CMAKE_CXX_LINK_FLAGS      "" CACHE STRING "" FORCE)

# 🚀 Re-Inject Mandatory SGRN Isolation Flags
set(SGRN_LINK_FLAGS "-fno-autolink -fuse-ld=lld")
if(SGRN_CROSS_WIN64)
    set(SGRN_LINK_FLAGS "${SGRN_LINK_FLAGS} ${SGRN_CROSS_LINK_FLAGS}")
endif()
set(CMAKE_EXE_LINKER_FLAGS    "${SGRN_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "${SGRN_LINK_FLAGS}")

# 🧪 Clean Link Templates
# Note: We must include -Wl,--out-implib for Windows/MinGW to generate .dll.a files
if(WIN32 OR MINGW OR SGRN_CROSS_WIN64)
    set(LINK_RULE_TEMPLATE "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,--out-implib,<TARGET_IMPLIB> <LINK_LIBRARIES>")
else()
    set(LINK_RULE_TEMPLATE "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
endif()

set(CMAKE_CXX_LINK_EXECUTABLE "${LINK_RULE_TEMPLATE}")
set(CMAKE_CXX_CREATE_SHARED_LIBRARY "${LINK_RULE_TEMPLATE} -shared")

if(WIN32 OR MINGW OR SGRN_CROSS_WIN64)
    set(C_LINK_RULE_TEMPLATE "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,--out-implib,<TARGET_IMPLIB> <LINK_LIBRARIES>")
else()
    set(C_LINK_RULE_TEMPLATE "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
endif()
set(CMAKE_C_LINK_EXECUTABLE "${C_LINK_RULE_TEMPLATE}")
set(CMAKE_C_CREATE_SHARED_LIBRARY "${C_LINK_RULE_TEMPLATE} -shared")

message(STATUS "[SGRN] Rule Overrides Applied (Stalling -defaultlib intrusion)")
