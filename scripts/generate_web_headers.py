#!/usr/bin/env python3
# =============================================================================
# generate_web_headers.py — COMPATIBILITY SHIM
# =============================================================================
# This script is kept for backwards-compatibility with CMakeLists.txt
# invocations that pre-date the unified VFS generator.
#
# All logic now lives in generate_embedded_assets.py.
# This shim translates the legacy CLI interface into a call to the new tool.
# =============================================================================
import sys
import subprocess
from pathlib import Path

def main():
    import argparse
    parser = argparse.ArgumentParser(description="[shim] Generate compressed C++ web-asset headers.")
    parser.add_argument("src_dir")
    parser.add_argument("out_dir")
    parser.add_argument("--level",     type=int, default=19)
    parser.add_argument("--use-bun",   action="store_true")
    parser.add_argument("--namespace", type=str, default="sgrn::datastore::assets::web")
    args = parser.parse_args()

    # Locate the unified generator relative to this file
    this_dir = Path(__file__).parent
    unified = this_dir / "generate_embedded_assets.py"

    cmd = [
        sys.executable, str(unified),
        args.src_dir, args.out_dir,
        "--namespace", args.namespace,
        "--kind",      "web",
        "--master",    "web_assets.hpp",
        "--level",     str(args.level),
    ]

    result = subprocess.run(cmd)
    sys.exit(result.returncode)

if __name__ == "__main__":
    main()
