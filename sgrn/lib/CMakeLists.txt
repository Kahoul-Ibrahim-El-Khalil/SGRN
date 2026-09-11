# SGRN libraries — one directory per component (headers + sources + tests).
# Added from sgrn/CMakeLists.txt after the global options/helpers above.
# Apps live in sibling sgrn/apps/ and configure after this directory so all
# library targets exist before packaging/install rules run.

# 1. Core & Utils (Foundational)
if(SGRN_BUILD_UTILS)
    add_subdirectory(core)
    add_subdirectory(codecs)
    add_subdirectory(utils)
endif()

# 2. Datastore Client & Shell (formerly SDK)
sgrn_add_component(SGRN_BUILD_SDK datastore/client)
sgrn_add_component(SGRN_BUILD_SDK datastore/shell)
sgrn_add_component(SGRN_BUILD_CODECS codecs)

# 3. SCL (Parsing layer)
sgrn_add_component(SGRN_BUILD_GATEWAY scl)

# 3.5 Scripting Engine
sgrn_add_component(SGRN_BUILD_GATEWAY scripting)

# 4. Gateway (Runtime)
sgrn_add_component(SGRN_BUILD_GATEWAY wrappers)
sgrn_add_component(SGRN_BUILD_GATEWAY common)
sgrn_add_component(SGRN_BUILD_GATEWAY gateway)

# 5. Virtual-PLC data plane (runtime + simulation + replay for s7shell and tests)
sgrn_add_component(SGRN_BUILD_S7 plcsim)

# 6. S7Shell (shell + script bindings over plcsim)
sgrn_add_component(SGRN_BUILD_S7 s7shell)

# 7. Backend (Infrastructure)
sgrn_add_component(SGRN_BUILD_DATASTORE datastore)
