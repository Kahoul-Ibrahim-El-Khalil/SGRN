#!/usr/bin/env python3

import os
import argparse
import re
from pathlib import Path


DEFAULT_EXCLUDES = {
    ".git",
    "__pycache__",
    "node_modules",
    "dist",
    "build",
    ".venv",
    "venv",
}


def is_hidden(path: Path) -> bool:
    return any(part.startswith(".") for part in path.parts)


def should_include(file_path: Path, extensions, exclude_dirs, regex_patterns, relative_path: str) -> bool:
    """
    Decide whether a file should be included based on:
      - hidden status (skip)
      - excluded directory names (skip)
      - extension whitelist (if any)
      - regex pattern match on relative path (if any)
    """
    # Ignore hidden files/directories
    if is_hidden(file_path):
        return False

    # Ignore excluded directories
    for part in file_path.parts:
        if part in exclude_dirs:
            return False

    # Extension filtering (if extensions are specified)
    if extensions and file_path.suffix.lower() not in extensions:
        return False

    # Regex filtering (if regex patterns are specified)
    if regex_patterns:
        # Match the relative path (string) against any pattern
        if not any(pattern.search(relative_path) for pattern in regex_patterns):
            return False

    return True


def aggregate_directory(
    directory,
    outfile,
    extensions,
    exclude_dirs,
    calling_directory,
    regex_patterns,
):

    directory = Path(directory).resolve()

    if not directory.exists():
        print(f"[!] Directory not found: {directory}")
        return 0

    total_files = 0

    for root, dirs, files in os.walk(directory):

        # Prevent traversal into hidden/excluded directories
        dirs[:] = [
            d for d in dirs
            if not d.startswith(".")
            and d not in exclude_dirs
        ]

        for file in sorted(files):

            # Skip hidden files
            if file.startswith("."):
                continue

            file_path = Path(root) / file

            # Compute relative path from invocation directory
            try:
                relative_path = str(file_path.relative_to(calling_directory))
            except ValueError:
                # Fallback: absolute path if not relative (shouldn't happen)
                relative_path = str(file_path)

            if not should_include(
                file_path,
                extensions,
                exclude_dirs,
                regex_patterns,
                relative_path,
            ):
                continue

            try:
                with open(
                    file_path,
                    "r",
                    encoding="utf-8",
                    errors="ignore"
                ) as infile:

                    separator = "=" * 100

                    outfile.write(
                        f"\n{separator}\n"
                        f"FILE: {relative_path}\n"
                        f"{separator}\n\n"
                    )

                    outfile.write(infile.read())
                    outfile.write("\n")

                    total_files += 1

                    print(f"[+] Added: {relative_path}")

            except Exception as e:
                print(f"[!] Failed: {file_path} -> {e}")

    return total_files


def main():

    parser = argparse.ArgumentParser(
        description="Recursive multi-directory file concatenator"
    )

    parser.add_argument(
        "directories",
        nargs="+",
        help="Directories to aggregate"
    )

    parser.add_argument(
        "--output",
        "-o",
        default="combined.txt",
        help="Output file"
    )

    parser.add_argument(
        "--ext",
        "-e",
        nargs="*",
        default=[],
        help="Extensions to include (example: .py .c .h .txt)"
    )

    parser.add_argument(
        "--exclude",
        nargs="*",
        default=list(DEFAULT_EXCLUDES),
        help="Directories to exclude"
    )

    parser.add_argument(
        "--include-regex",
        "-r",
        nargs="*",
        default=[],
        help="Regex patterns to match against relative file path. "
             "If provided, only files matching any pattern are included. "
             "If both --ext and --include-regex are given, both must match."
    )

    args = parser.parse_args()

    extensions = {e.lower() for e in args.ext}
    regex_patterns = [re.compile(p) for p in args.include_regex]
    exclude_dirs = set(args.exclude)

    calling_directory = Path.cwd().resolve()

    total_files = 0

    with open(args.output, "w", encoding="utf-8") as outfile:

        for directory in args.directories:

            total_files += aggregate_directory(
                directory=directory,
                outfile=outfile,
                extensions=extensions,
                exclude_dirs=exclude_dirs,
                calling_directory=calling_directory,
                regex_patterns=regex_patterns,
            )

    print(
        f"\n[✓] Aggregated {total_files} files into: {args.output}"
    )


if __name__ == "__main__":
    main()
