#!/usr/bin/env python3
"""
scripts/rename_snake_to_kebab.py

Automates migrating C++ source and header files from snake_case to kebab-case:
  1. Identifies files with '_' in their stem (e.g., secrets_wizard.hpp -> secrets-wizard.hpp).
  2. Updates all `#include <...>` and `#include "..."` statements across the codebase.
  3. Updates CMakeLists.txt target and file listings.
  4. Moves files via `git mv` (or standard filesystem rename for untracked files).

Safety:
  - Defaults to --dry-run so you can inspect all planned renames and header edits.
  - Automatically skips Python directories (sgrn/python), Drogon ORM models (orm/models),
    third-party dependencies (.prefix, node_modules, build), and .git.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

# Directories to strictly exclude from renaming
EXCLUDED_DIRS = {
    ".git",
    ".build",
    "build",
    "node_modules",
    "__pycache__",
    ".prefix",
    ".dist",
    "sgrn/python",           # Python imports forbid hyphens!
    "orm/models",            # Drogon ORM auto-generated models
    "dist",
}

# C/C++ extensions to rename
TARGET_EXTENSIONS = {".hpp", ".cpp", ".h", ".cc", ".cxx"}

# Extensions to scan for #include updates
SCAN_EXTENSIONS = {".hpp", ".cpp", ".h", ".cc", ".cxx", ".txt", ".cmake", ".sh"}


def is_excluded(path_str: str) -> bool:
    normalized = path_str.replace("\\", "/")
    for excluded in EXCLUDED_DIRS:
        if f"/{excluded}/" in f"/{normalized}/" or normalized.startswith(f"{excluded}/"):
            return True
    return False


def to_kebab_case(name: str) -> str:
    """Convert snake_case or mixed Camel_snake to kebab-case."""
    # Replace underscores with hyphens
    return name.replace("_", "-")


def collect_renames(root_dir: Path) -> dict[Path, Path]:
    """Find all eligible C++ files containing '_' in their filename."""
    renames = {}
    for dirpath, dirnames, filenames in os.walk(root_dir):
        rel_dir = os.path.relpath(dirpath, root_dir)
        if is_excluded(rel_dir):
            dirnames.clear()
            continue

        for f in filenames:
            p = Path(dirpath) / f
            rel_p = p.relative_to(root_dir)
            if is_excluded(str(rel_p)):
                continue

            if p.suffix in TARGET_EXTENSIONS and "_" in p.stem:
                new_stem = to_kebab_case(p.stem)
                new_name = f"{new_stem}{p.suffix}"
                new_path = p.parent / new_name
                renames[p] = new_path

    return renames


def update_references(root_dir: Path, rename_map: dict[str, str], dry_run: bool) -> int:
    """Update #include and CMake references matching renamed filenames."""
    edits = 0
    # Precompile regex for fast replacement
    # Match patterns like:
    #   #include <sgrn/.../old_name.hpp>
    #   #include "old_name.hpp"
    #   add_executable(... old_name.cpp)
    for dirpath, dirnames, filenames in os.walk(root_dir):
        rel_dir = os.path.relpath(dirpath, root_dir)
        if is_excluded(rel_dir):
            dirnames.clear()
            continue

        for f in filenames:
            p = Path(dirpath) / f
            if p.suffix not in SCAN_EXTENSIONS and p.name != "CMakeLists.txt":
                continue

            try:
                content = p.read_text(encoding="utf-8")
            except Exception:
                continue

            modified = False
            new_content = content
            for old_name, new_name in rename_map.items():
                if old_name in new_content:
                    # Replace in includes and CMake entries
                    pattern = re.compile(rf"(?<=[/<\"\s\(\;\']){re.escape(old_name)}(?=[>\"\s\n\)\;\,\'])")
                    if pattern.search(new_content):
                        new_content = pattern.sub(new_name, new_content)
                        modified = True

            if modified:
                edits += 1
                if dry_run:
                    print(f"  [REF UPDATE] {p.relative_to(root_dir)}")
                else:
                    p.write_text(new_content, encoding="utf-8")

    return edits


def is_git_tracked(path: Path) -> bool:
    try:
        res = subprocess.run(
            ["git", "ls-files", "--error-unmatch", str(path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return res.returncode == 0
    except Exception:
        return False


def main():
    parser = argparse.ArgumentParser(description="Migrate C++ file names from snake_case to kebab-case.")
    parser.add_argument("--target", default="sgrn/lib/datastore", help="Directory to process (default: sgrn/lib/datastore)")
    parser.add_argument("--execute", action="store_true", help="Perform the renames and file updates (default is dry-run)")
    args = parser.parse_args()

    root_dir = Path.cwd()
    target_dir = root_dir / args.target
    if not target_dir.exists():
        print(f"Error: target directory '{target_dir}' does not exist.")
        sys.exit(1)

    dry_run = not args.execute
    mode_str = "DRY-RUN (no files will be changed)" if dry_run else "EXECUTE"
    print(f"=== Snake to Kebab Case Migration [{mode_str}] ===")
    print(f"Target: {target_dir.relative_to(root_dir)}\n")

    renames = collect_renames(target_dir)
    if not renames:
        print("No files with snake_case names found in target directory.")
        return

    print(f"Found {len(renames)} file(s) to rename:\n")
    rename_str_map = {}
    for old_p, new_p in sorted(renames.items()):
        rel_old = old_p.relative_to(root_dir)
        rel_new = new_p.relative_to(root_dir)
        rename_str_map[old_p.name] = new_p.name
        print(f"  {rel_old}  ->  {rel_new.name}")

    print("\nScanning for #include and build references...")
    ref_edits = update_references(root_dir, rename_str_map, dry_run)
    print(f"References to update in {ref_edits} file(s).")

    if dry_run:
        print("\n[DRY RUN COMPLETE] To apply these changes, run with --execute:")
        print(f"  python3 scripts/rename_snake_to_kebab.py --target {args.target} --execute")
        return

    print("\nApplying renames...")
    for old_p, new_p in renames.items():
        if is_git_tracked(old_p):
            subprocess.run(["git", "mv", str(old_p), str(new_p)], check=True)
        else:
            old_p.rename(new_p)

    print("Migration complete!")


if __name__ == "__main__":
    main()
