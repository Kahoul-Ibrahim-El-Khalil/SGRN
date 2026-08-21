#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
from pathlib import Path


DEFAULT_EXCLUDES = {
    ".git",
    "__pycache__",
    "node_modules",
    ".dist",
    ".prefix",
    ".build",
    ".venv",
    "venv",
}


class GitIgnoreFilter:
    """
    Uses Git itself to determine whether paths are ignored.

    This respects:
      - .gitignore
      - nested .gitignore files
      - .git/info/exclude
      - Git global excludes
      - Git's complete ignore pattern semantics

    Tracked files are not considered ignored by git check-ignore.
    """

    def __init__(self, directory: Path):
        self.directory = directory
        self.repo_root = self._find_repo_root(directory)

    @staticmethod
    def _find_repo_root(directory: Path):
        try:
            result = subprocess.run(
                [
                    "git",
                    "-C",
                    str(directory),
                    "rev-parse",
                    "--show-toplevel",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                check=False,
            )

            if result.returncode != 0:
                return None

            return Path(result.stdout.strip()).resolve()

        except FileNotFoundError:
            return None

    @property
    def enabled(self):
        return self.repo_root is not None

    def ignored(self, path: Path) -> bool:
        """
        Return True if Git considers the path ignored.
        """

        if not self.enabled:
            return False

        try:
            relative_path = path.resolve().relative_to(self.repo_root)

            result = subprocess.run(
                [
                    "git",
                    "-C",
                    str(self.repo_root),
                    "check-ignore",
                    "--quiet",
                    "--no-index",
                    "--",
                    str(relative_path),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )

            return result.returncode == 0

        except ValueError:
            return False


def is_hidden(path: Path) -> bool:
    return any(part.startswith(".") for part in path.parts)


def should_include(
    file_path: Path,
    extensions,
    exclude_dirs,
    regex_patterns,
    relative_path: str,
    gitignore,
) -> bool:

    # Ignore hidden files/directories.
    if is_hidden(file_path):
        return False

    # Ignore explicitly excluded directories.
    for part in file_path.parts:
        if part in exclude_dirs:
            return False

    # Ignore anything Git ignores.
    if gitignore.ignored(file_path):
        return False

    # Extension filtering.
    if extensions and file_path.suffix.lower() not in extensions:
        return False

    # Regex filtering.
    if regex_patterns:
        if not any(
            pattern.search(relative_path)
            for pattern in regex_patterns
        ):
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

    if not directory.is_dir():
        print(f"[!] Not a directory: {directory}")
        return 0

    gitignore = GitIgnoreFilter(directory)

    if gitignore.enabled:
        print(f"[*] Git repository: {gitignore.repo_root}")
    else:
        print(f"[*] Not inside a Git repository: {directory}")

    total_files = 0

    for root, dirs, files in os.walk(directory):

        root_path = Path(root)

        # --------------------------------------------------------------
        # Filter directories before os.walk enters them.
        # --------------------------------------------------------------

        filtered_dirs = []

        for dirname in dirs:

            dir_path = root_path / dirname

            # Hidden directories.
            if dirname.startswith("."):
                continue

            # Explicit exclusions.
            if dirname in exclude_dirs:
                continue

            # Git ignored directories.
            if gitignore.ignored(dir_path):
                print(f"[-] Git ignored: {dir_path}")
                continue

            filtered_dirs.append(dirname)

        dirs[:] = filtered_dirs

        # --------------------------------------------------------------
        # Files
        # --------------------------------------------------------------

        for filename in sorted(files):

            if filename.startswith("."):
                continue

            file_path = root_path / filename

            try:
                relative_path = str(
                    file_path.relative_to(calling_directory)
                )
            except ValueError:
                relative_path = str(file_path)

            if not should_include(
                file_path=file_path,
                extensions=extensions,
                exclude_dirs=exclude_dirs,
                regex_patterns=regex_patterns,
                relative_path=relative_path,
                gitignore=gitignore,
            ):
                continue

            try:
                with open(
                    file_path,
                    "r",
                    encoding="utf-8",
                    errors="ignore",
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
                print(
                    f"[!] Failed: {file_path} -> {e}"
                )

    return total_files


def main():

    parser = argparse.ArgumentParser(
        description=(
            "Recursive multi-directory file concatenator "
            "with Git ignore support"
        )
    )

    # ------------------------------------------------------------------
    # Directories
    # ------------------------------------------------------------------

    parser.add_argument(
        "-d",
        "--directory",
        dest="directories",
        action="append",
        required=True,
        metavar="DIR",
        help=(
            "Directory to aggregate. "
            "Can be specified multiple times."
        ),
    )

    # ------------------------------------------------------------------
    # Output
    # ------------------------------------------------------------------

    parser.add_argument(
        "-o",
        "--output",
        default="combined.txt",
        metavar="FILE",
        help="Output file (default: combined.txt)",
    )

    # ------------------------------------------------------------------
    # Extensions
    # ------------------------------------------------------------------

    parser.add_argument(
        "-e",
        "--ext",
        dest="extensions",
        action="append",
        default=[],
        metavar="EXT",
        help=(
            "Extension to include. "
            "Can be specified multiple times."
        ),
    )

    # ------------------------------------------------------------------
    # Explicit directory exclusions
    # ------------------------------------------------------------------

    parser.add_argument(
        "-x",
        "--exclude",
        dest="excludes",
        action="append",
        default=None,
        metavar="DIR",
        help=(
            "Directory name to exclude in addition to Git ignores. "
            "Can be specified multiple times."
        ),
    )

    # ------------------------------------------------------------------
    # Regex
    # ------------------------------------------------------------------

    parser.add_argument(
        "-r",
        "--include-regex",
        dest="include_regex",
        action="append",
        default=[],
        metavar="REGEX",
        help=(
            "Regex to match against relative file paths. "
            "Can be specified multiple times."
        ),
    )

    args = parser.parse_args()

    # ------------------------------------------------------------------
    # Normalize extensions
    # ------------------------------------------------------------------

    extensions = {
        ext.lower()
        if ext.startswith(".")
        else f".{ext.lower()}"
        for ext in args.extensions
    }

    # ------------------------------------------------------------------
    # Compile regex patterns
    # ------------------------------------------------------------------

    try:
        regex_patterns = [
            re.compile(pattern)
            for pattern in args.include_regex
        ]
    except re.error as e:
        parser.error(f"Invalid regular expression: {e}")

    # ------------------------------------------------------------------
    # Explicit excludes
    # ------------------------------------------------------------------

    if args.excludes is None:
        exclude_dirs = set(DEFAULT_EXCLUDES)
    else:
        exclude_dirs = set(args.excludes)

    # ------------------------------------------------------------------
    # Aggregate
    # ------------------------------------------------------------------

    calling_directory = Path.cwd().resolve()
    total_files = 0

    try:
        with open(
            args.output,
            "w",
            encoding="utf-8",
        ) as outfile:

            for directory in args.directories:

                total_files += aggregate_directory(
                    directory=directory,
                    outfile=outfile,
                    extensions=extensions,
                    exclude_dirs=exclude_dirs,
                    calling_directory=calling_directory,
                    regex_patterns=regex_patterns,
                )

    except OSError as e:
        parser.error(
            f"Could not open output file "
            f"'{args.output}': {e}"
        )

    print(
        f"\n[✓] Aggregated "
        f"{total_files} files into: "
        f"{args.output}"
    )


if __name__ == "__main__":
    main()
