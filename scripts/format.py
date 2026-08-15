#!/usr/bin/env python3
import os
import subprocess
import sys
import argparse

# ── Configuration ─────────────────────────────────────────────────────────────
# Directories to strictly exclude from formatting (only when walking dirs)
EXCLUDE_DIRS = {
    'extern',
    'build',
    'dist',
    'node_modules',
    '.git',
    '.micromamba',
    '.gemini'
}

# File extensions per language
EXTENSIONS = {
    'cpp': ['.cpp', '.hpp', '.c', '.h', '.cxx', '.hxx'],
    'ts': ['.ts', '.tsx', '.js', '.jsx', '.json', '.css', '.scss']
}

def find_files(lang, paths=None):
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

def format_cpp(paths=None):
    """Formats C++ files using clang-format."""
    files = find_files('cpp', paths)
    if not files:
        print("[format] No C++ files found.")
        return

    print(f"[format] Formatting {len(files)} C++ files...")
    try:
        subprocess.run(['clang-format', '-i'] + files, check=True)
        print("[format] C++ formatting complete.")
    except FileNotFoundError:
        print("[error] 'clang-format' not found in PATH. Please install it.")
    except subprocess.CalledProcessError as e:
        print(f"[error] clang-format failed: {e}")

def format_ts(paths=None):
    """Formats TypeScript/JS files using prettier."""
    files = find_files('ts', paths)
    if not files:
        print("[format] No TS/JS files found.")
        return

    print(f"[format] Formatting {len(files)} TS/JS files...")
    try:
        subprocess.run(['npx', 'prettier', '--write'] + files, check=True)
        print("[format] TS/JS formatting complete.")
    except FileNotFoundError:
        print("[error] 'npx' (Node.js) not found in PATH. Please install Node.js.")
    except subprocess.CalledProcessError as e:
        print(f"[error] prettier failed: {e}")

def main():
    parser = argparse.ArgumentParser(description="SGRN Code Formatter")
    parser.add_argument('lang', choices=['cpp', 'ts', 'all'],
                        help="Language to format")
    parser.add_argument('--dir', '-d', action='append',
                        help="Directory to scan for files (can be used multiple times)")
    parser.add_argument('--files', '-f', action='append',
                        help="Specific files to format (can be used multiple times)")

    args = parser.parse_args()

    # Build list of paths from arguments; default to current directory
    paths = []
    if args.dir:
        paths.extend(args.dir)
    if args.files:
        paths.extend(args.files)
    if not paths:
        paths = ['.']

    if args.lang == 'cpp' or args.lang == 'all':
        format_cpp(paths)

    if args.lang == 'ts' or args.lang == 'all':
        format_ts(paths)

if __name__ == "__main__":
    main()
