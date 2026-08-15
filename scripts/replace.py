#!/usr/bin/env python3
"""
replace.py - Replace a literal string in files.

Examples:

    # Replace in a single file
    python replace.py -f main.c --old foo --new bar

    # Replace in multiple files
    python replace.py -f main.c util.c include/header.h --old foo --new bar

    # Replace recursively in a directory
    python replace.py -d src --old foo --new bar

    # Replace recursively in a directory, only in C/C++ files
    python replace.py -d src \
        --extensions .c .h .cpp .hpp \
        --old foo \
        --new bar

    # Extensions may be specified without dots
    python replace.py -d src \
        --extensions c h cpp hpp \
        --old foo \
        --new bar
"""

import argparse
from pathlib import Path

parser = argparse.ArgumentParser(
    description="Replace a literal string in files."
)

target = parser.add_mutually_exclusive_group(required=True)
target.add_argument(
    "-f", "--files",
    nargs="+",
    metavar="FILE",
    help="One or more files to modify"
)
target.add_argument(
    "-d", "--directory",
    metavar="DIR",
    help="Directory to process recursively"
)

parser.add_argument(
    "--old",
    required=True,
    help="Literal string to replace"
)

parser.add_argument(
    "--new",
    required=True,
    help="Replacement string"
)

parser.add_argument(
    "--extensions",
    nargs="+",
    metavar="EXT",
    help="Only process files with these extensions (e.g. .c .h .cpp)"
)

args = parser.parse_args()

extensions = None
if args.extensions:
    extensions = {
        ext if ext.startswith(".") else f".{ext}"
        for ext in args.extensions
    }

if args.files:
    files = [Path(f) for f in args.files]
else:
    files = [
        p for p in Path(args.directory).rglob("*")
        if p.is_file()
        and (extensions is None or p.suffix.lower() in extensions)
    ]

for path in files:
    if extensions is not None and path.suffix.lower() not in extensions:
        continue

    try:
        content = path.read_text(encoding="utf-8")
        updated = content.replace(args.old, args.new)

        if updated != content:
            path.write_text(updated, encoding="utf-8")
            print(f"Updated: {path}")

    except Exception as e:
        print(f"Skipped {path}: {e}")
