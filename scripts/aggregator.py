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

    Respects Git's ignore rules, including:
      - .gitignore
      - nested .gitignore files
      - .git/info/exclude
      - Git global excludes

    Git is queried with --no-index so paths are evaluated against ignore
    patterns regardless of whether they are tracked.
    """

    def __init__(self, directory: Path):
        self.directory = directory
        self.repo_root = self._find_repo_root(directory)

    @staticmethod
    def _find_repo_root(directory: Path) -> Path | None:
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

            root = result.stdout.strip()
            return Path(root).resolve() if root else None

        except (FileNotFoundError, OSError):
            return None

    @property
    def enabled(self) -> bool:
        return self.repo_root is not None

    def ignored(self, path: Path) -> bool:
        """Return True if Git considers the path ignored."""
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

        except (ValueError, OSError):
            return False


def is_hidden(path: Path) -> bool:
    return any(part.startswith(".") for part in path.parts)


def should_include(
    file_path: Path,
    extensions: set[str],
    exclude_dirs: set[str],
    regex_patterns: list[re.Pattern],
    relative_path: str,
    gitignore: GitIgnoreFilter,
    output_path: Path,
) -> bool:
    # Never aggregate the output file itself.
    try:
        if file_path.resolve() == output_path:
            return False
    except OSError:
        pass

    # Never follow/include symlinked files unless explicitly supported.
    if file_path.is_symlink():
        return False

    # Ignore hidden files/directories.
    if is_hidden(file_path):
        return False

    # Ignore explicitly excluded directories.
    if any(part in exclude_dirs for part in file_path.parts):
        return False

    # Ignore anything Git ignores.
    if gitignore.ignored(file_path):
        return False

    # Extension filtering.
    if extensions and file_path.suffix.lower() not in extensions:
        return False

    # Regex filtering.
    if regex_patterns and not any(
        pattern.search(relative_path) for pattern in regex_patterns
    ):
        return False

    return True


def aggregate_directory(
    directory: Path,
    outfile,
    extensions: set[str],
    exclude_dirs: set[str],
    regex_patterns: list[re.Pattern],
    output_path: Path,
) -> int:
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

    def on_walk_error(error: OSError) -> None:
        print(f"[!] Cannot access {error.filename or directory}: {error}")

    for root, dirs, files in os.walk(directory, onerror=on_walk_error):
        root_path = Path(root)

        # Filter directories before os.walk enters them.
        filtered_dirs = []

        for dirname in sorted(dirs):
            dir_path = root_path / dirname

            if dirname.startswith("."):
                continue

            if dirname in exclude_dirs:
                continue

            if dir_path.is_symlink():
                continue

            if gitignore.ignored(dir_path):
                print(f"[-] Git ignored: {dir_path}")
                continue

            filtered_dirs.append(dirname)

        dirs[:] = filtered_dirs

        # Files
        for filename in sorted(files):
            if filename.startswith("."):
                continue

            file_path = root_path / filename

            try:
                relative_path = str(file_path.relative_to(directory))
            except ValueError:
                relative_path = str(file_path)

            if not should_include(
                file_path=file_path,
                extensions=extensions,
                exclude_dirs=exclude_dirs,
                regex_patterns=regex_patterns,
                relative_path=relative_path,
                gitignore=gitignore,
                output_path=output_path,
            ):
                continue

            try:
                with open(
                    file_path,
                    "r",
                    encoding="utf-8",
                    errors="strict",
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

            except UnicodeDecodeError as e:
                print(f"[!] Skipped non-UTF-8 file: {file_path} -> {e}")
            except (OSError, UnicodeError) as e:
                print(f"[!] Failed: {file_path} -> {e}")

    return total_files


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Recursive multi-directory file concatenator "
            "with Git ignore support"
        )
    )

    # Directories
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

    # Output
    parser.add_argument(
        "-o",
        "--output",
        default="combined.txt",
        metavar="FILE",
        help="Output file (default: combined.txt)",
    )

    # Extensions
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

    # Explicit directory exclusions
    parser.add_argument(
        "-x",
        "--exclude",
        dest="excludes",
        action="append",
        default=[],
        metavar="DIR",
        help=(
            "Directory name to exclude in addition to the default "
            "and Git ignores. Can be specified multiple times."
        ),
    )

    # Regex
    parser.add_argument(
        "-r",
        "--include-regex",
        dest="include_regex",
        action="append",
        default=[],
        metavar="REGEX",
        help=(
            "Regex to match against paths relative to each input directory. "
            "Can be specified multiple times."
        ),
    )

    args = parser.parse_args()

    # Normalize extensions.
    extensions = {
        ext.lower() if ext.startswith(".") else f".{ext.lower()}"
        for ext in args.extensions
    }

    # Compile regex patterns.
    try:
        regex_patterns = [
            re.compile(pattern)
            for pattern in args.include_regex
        ]
    except re.error as e:
        parser.error(f"Invalid regular expression: {e}")

    # Explicit excludes extend the defaults.
    exclude_dirs = set(DEFAULT_EXCLUDES)
    exclude_dirs.update(args.excludes)

    calling_directory = Path.cwd().resolve()
    output_path = Path(args.output).resolve()

    # Prevent accidentally placing the output inside a directory that will
    # subsequently be aggregated.
    output_path.parent.mkdir(parents=True, exist_ok=True)

    total_files = 0

    try:
        with open(
            output_path,
            "w",
            encoding="utf-8",
            errors="strict",
        ) as outfile:
            for directory in args.directories:
                total_files += aggregate_directory(
                    directory=Path(directory),
                    outfile=outfile,
                    extensions=extensions,
                    exclude_dirs=exclude_dirs,
                    regex_patterns=regex_patterns,
                    output_path=output_path,
                )

    except OSError as e:
        parser.error(
            f"Could not open output file "
            f"'{output_path}': {e}"
        )

    print(
        f"\n[✓] Aggregated "
        f"{total_files} files into: "
        f"{output_path}"
    )


if __name__ == "__main__":
    main()
