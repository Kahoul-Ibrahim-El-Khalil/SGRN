#!/usr/bin/env python3
import os
import subprocess
import sys
import argparse
import shutil
from pathlib import Path

# ── Configuration ─────────────────────────────────────────────────────────────
# Directories to strictly exclude from formatting (only when walking dirs)
EXCLUDE_DIRS = {
    'extern',
    'build',
    '.build',
    'dist',
    '.dist',
    'node_modules',
    '.git',
    '.micromamba',
    '.gemini'
}

# File extensions per language
EXTENSIONS = {
    'cpp': ['.cpp', '.hpp', '.c', '.h', '.cxx', '.hxx', '.cc'],
    'ts': ['.ts', '.tsx', '.js', '.jsx', '.json', '.css', '.scss']
}

def findFiles(lang, paths=None):
    """
    Finds all relevant files for a language, given a list of paths.
    Paths can be directories or individual files.
    If paths is None or empty, defaults to ['.'].
    """
    if not paths:
        paths = ['.']

    targets = []
    exts = EXTENSIONS.get(lang, [])

    for path in paths:
        if os.path.isfile(path):
            if any(path.endswith(ext) for ext in exts):
                targets.append(path)
        elif os.path.isdir(path):
            for root, dirs, files in os.walk(path):
                # Filter out excluded directories in‑place to prevent walking into them
                dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
                for file in files:
                    if any(file.endswith(ext) for ext in exts):
                        targets.append(os.path.join(root, file))
        else:
            print(f"[warning] Path not found: {path}")

    return targets

def findClangFormat(custom_bin=None) -> str | None:
    """Locates a suitable clang-format binary."""
    if custom_bin and os.path.isfile(custom_bin) and os.access(custom_bin, os.X_OK):
        return custom_bin

    which_bin = shutil.which("clang-format")
    if which_bin:
        return which_bin

    # Search versioned clang-format in PATH (e.g. clang-format-22 down to 14)
    for v in range(25, 13, -1):
        which_v = shutil.which(f"clang-format-{v}")
        if which_v:
            return which_v

    # Search active conda / micromamba prefix
    conda_prefix = os.environ.get("CONDA_PREFIX")
    if conda_prefix:
        for sub in ["bin", "Library/bin"]:
            p = os.path.join(conda_prefix, sub, "clang-format")
            if os.path.isfile(p) and os.access(p, os.X_OK):
                return p
            for v in range(25, 13, -1):
                pv = os.path.join(conda_prefix, sub, f"clang-format-{v}")
                if os.path.isfile(pv) and os.access(pv, os.X_OK):
                    return pv

    # Search common user / system install directories
    home = os.path.expanduser("~")
    candidates = [
        os.path.join(home, "micromamba", "envs", "SGRN", "bin", "clang-format"),
        os.path.join(home, ".micromamba", "envs", "SGRN", "bin", "clang-format"),
        "/usr/bin/clang-format",
        "/usr/local/bin/clang-format",
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
        for v in range(25, 13, -1):
            cv = f"{c}-{v}"
            if os.path.isfile(cv) and os.access(cv, os.X_OK):
                return cv

    return None

def formatCpp(paths=None, clang_format_bin=None):
    """Formats C++ files using clang-format."""
    files = findFiles('cpp', paths)
    if not files:
        print("[format] No C++ files found.")
        return

    bin_path = findClangFormat(clang_format_bin)
    if not bin_path:
        print("[error] 'clang-format' not found in PATH or standard environments. Skipping C++ formatting.")
        return

    print(f"[format] Formatting {len(files)} C++ files with {bin_path}...")
    chunk_size = 50
    try:
        for i in range(0, len(files), chunk_size):
            chunk = files[i:i + chunk_size]
            subprocess.run([bin_path, '-i'] + chunk, check=True)
        print("[format] C++ formatting complete.")
    except subprocess.CalledProcessError as e:
        print(f"[error] clang-format failed: {e}")

def formatTs(paths=None):
    """Formats TypeScript/JS files using prettier."""
    files = findFiles('ts', paths)
    if not files:
        print("[format] No TS/JS files found.")
        return

    print(f"[format] Formatting {len(files)} TS/JS files...")
    chunk_size = 50
    try:
        for i in range(0, len(files), chunk_size):
            chunk = files[i:i + chunk_size]
            subprocess.run(['npx', 'prettier', '--write'] + chunk, check=True)
        print("[format] TS/JS formatting complete.")
    except FileNotFoundError:
        print("[error] 'npx' (Node.js) not found in PATH. Please install Node.js.")
    except subprocess.CalledProcessError as e:
        print(f"[error] prettier failed: {e}")

# Backwards compatibility aliases
find_files = findFiles
format_cpp = formatCpp
format_ts = formatTs

def main():
    parser = argparse.ArgumentParser(description="SGRN Code Formatter")
    parser.add_argument('lang', choices=['cpp', 'ts', 'all'],
                        help="Language to format")
    parser.add_argument('--dir', '-d', action='append',
                        help="Directory to scan for files (can be used multiple times)")
    parser.add_argument('--files', '-f', action='append',
                        help="Specific files to format (can be used multiple times)")
    parser.add_argument('--clang-format', default=None,
                        help="Explicit path to clang-format binary")

    args = parser.parse_args()

    # Build list of paths from arguments; default to current directory
    paths = []
    if args.dir:
        paths.extend(args.dir)
    if args.files:
        paths.extend(args.files)
    if not paths:
        paths = ['.']

    if args.lang in ('cpp', 'all'):
        formatCpp(paths, args.clang_format)

    if args.lang in ('ts', 'all'):
        formatTs(paths)

if __name__ == "__main__":
    main()
