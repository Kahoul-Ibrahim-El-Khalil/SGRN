#!/usr/bin/env python3
"""Scaffold a new SGRN app or library following the repo conventions.

Usage:
    scripts/scaffold.py app <name> [--dest sgrn]
    scripts/scaffold.py lib <name> [--header-only] [--dest sgrn]

Creates the directory layout + boilerplate and prints the manual follow-up
checklist (gate entries, packaging/staging, tests). Refuses to overwrite.
See sgrn/apps/README.md and sgrn/lib/README.md for the full procedure.
"""

import argparse
import os
import sys

APP_CMAKELISTS = """# {Name} — TODO: one-line role description.
sgrn_add_component_executable(
    TARGET      {name}
    SOURCES     main.cpp
    ENABLE_ORIGIN_RPATH
    PRIVATE_LIBS
        sgrn_utils
        # TODO: more libraries as needed
    WIN_LIBS
        ${{WIN_LIBS_LIST}}
)
target_include_directories({name} PRIVATE
    ${{CMAKE_SOURCE_DIR}}/sgrn/lib/<lib>/include
)
sgrn_target({name})
sgrn_install_component_executable(TARGET {name} COMPONENT TODO-COMPONENT)
"""

APP_MAIN = """// {name} — TODO: describe what this entry point does.
#include <sgrn/utils/app.hpp>

#include <cstdio>

int main_cb(int /*argc*/, char** /*argv*/) {{
    // TODO: implement.
    std::printf("{name}: not implemented yet\\n");
    return EXIT_FAILURE;
}}

int main(int t_argc, char** t_argv) {{
    return sgrn::utils::app::runMain(t_argc, t_argv, main_cb, "{Name}");
}}
"""

APP_README = """# {name}

TODO: one-line role description.

## Role

TODO: what this entry point does and why it exists.

## Usage

```text
TODO: synopsis (quote `--help` output once implemented)
```

## Configuration

TODO: config files / flags, or "none".

## Dependencies

TODO: linked libraries and what each provides (see CMakeLists.txt).
"""

LIB_CMAKELISTS = """project(sgrn_{name} LANGUAGES CXX)

include(${{CMAKE_SOURCE_DIR}}/cmake/core.cmake)

set(SGRN_{NAME}_SOURCES
    src/{name}.cpp
)

sgrn_add_component_library(
    TARGET      sgrn_{name}
    ALIAS       sgrn::{name}
    TYPE        STATIC
    SOURCES     ${{SGRN_{NAME}_SOURCES}}
    INCLUDE_DIR ${{CMAKE_CURRENT_SOURCE_DIR}}/include
    PUBLIC_LIBS
        # TODO: e.g. sgrn::common sgrn_utils
)

sgrn_target(sgrn_{name})
"""

LIB_HEADER = """#pragma once

namespace sgrn::{name}
{{
// TODO: public surface.
}} // namespace sgrn::{name}
"""

LIB_SOURCE = """#include <sgrn/{name}/{name}.hpp>

namespace sgrn::{name}
{{
// TODO: implementation.
}} // namespace sgrn::{name}
"""

LIB_TEST = """// Smoke test wiring the new library into ctest (extend freely).
#include <cstdio>

int main() {{
    std::printf("{name}: smoke OK\\n");
    return 0;
}}
"""

LIB_TEST_CMAKE = """
if(SGRN_BUILD_TESTS)
    sgrn_add_component_executable(
        TARGET {name}_smoke_test
        SOURCES tests/{name}_smoke_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_{lname}
    )
    add_test(NAME {name}_smoke_test COMMAND {name}_smoke_test)
    sgrn_target({name}_smoke_test)
endif()
"""

HEADER_ONLY_CMAKELISTS = """project(sgrn_{name} LANGUAGES CXX)

add_library(sgrn_{name} INTERFACE)
add_library(sgrn::{name} ALIAS sgrn_{name})

target_include_directories(sgrn_{name} INTERFACE
    $<BUILD_INTERFACE:${{CMAKE_CURRENT_SOURCE_DIR}}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sgrn_{name} INTERFACE
    # TODO: header-only deps, e.g. sgrn::common
)

sgrn_install_component_library(
    TARGET sgrn_{name}
    EXPORT_NAME sgrn_{name}
    INCLUDE_SOURCE ${{CMAKE_CURRENT_SOURCE_DIR}}/include/sgrn
    INCLUDE_DESTINATION include
)
"""


def write_file(path, content):
    if os.path.exists(path):
        print(f"refusing to overwrite existing {path}", file=sys.stderr)
        sys.exit(1)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)
    print(f"created {path}")


def scaffold_app(root, name):
    base = os.path.join(root, "apps", name)
    write_file(os.path.join(base, "main.cpp"), APP_MAIN.format(name=name, Name=name.capitalize()))
    write_file(os.path.join(base, "CMakeLists.txt"), APP_CMAKELISTS.format(name=name, Name=name.capitalize()))
    write_file(os.path.join(base, "README.md"), APP_README.format(name=name))
    print("next steps:")
    print(f"  1. add_subdirectory({name}) to {root}/apps/CMakeLists.txt under the matching SGRN_BUILD_* gate")
    print("  2. add the target to a sgrn_package_runtime_dependencies bundle there")
    print("  3. if integration-tested, register the binary path in tests/ts/ or tests/run_tests.py")


def scaffold_lib(root, name, header_only):
    base = os.path.join(root, "lib", name)
    lname = name
    if header_only:
        write_file(os.path.join(base, "CMakeLists.txt"), HEADER_ONLY_CMAKELISTS.format(name=name))
    else:
        write_file(os.path.join(base, "CMakeLists.txt"),
                   LIB_CMAKELISTS.format(name=name, NAME=name.upper()) + LIB_TEST_CMAKE.format(name=name, lname=lname))
        write_file(os.path.join(base, "src", f"{name}.cpp"), LIB_SOURCE.format(name=name))
        write_file(os.path.join(base, "tests", f"{name}_smoke_test.cpp"), LIB_TEST.format(name=name))
    write_file(os.path.join(base, "include", "sgrn", name, f"{name}.hpp"), LIB_HEADER.format(name=name))
    print("next steps:")
    print(f"  1. sgrn_add_component(<OPTION> {name}) in {root}/lib/CMakeLists.txt (declare the SGRN_BUILD_* option if new)")
    print("  2. stage the lib + headers in cmake/staging_sgrn.cmake (else .prefix/ silently skips it)")
    print("  3. if it introduces cross-directory error types, extend scripts/lints/check_boundaries.sh")


def main():
    ap = argparse.ArgumentParser(description="Scaffold a new SGRN app or library.")
    ap.add_argument("kind", choices=["app", "lib"])
    ap.add_argument("name", help="directory/target base name, e.g. modbus (lib sgrn_modbus) or mbproxy")
    ap.add_argument("--header-only", action="store_true", help="lib: INTERFACE header-only project (core/common-style)")
    ap.add_argument("--dest", default="sgrn", help="monorepo sgrn/ directory (default: sgrn)")
    args = ap.parse_args()

    if "/" in args.name or args.name in (".", ".."):
        print("name must be a single directory name", file=sys.stderr)
        return 1
    if args.kind == "app":
        if args.header_only:
            print("--header-only only applies to lib", file=sys.stderr)
            return 1
        scaffold_app(args.dest, args.name)
    else:
        scaffold_lib(args.dest, args.name, args.header_only)
    return 0


if __name__ == "__main__":
    sys.exit(main())
