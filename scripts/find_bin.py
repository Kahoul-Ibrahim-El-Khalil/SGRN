"""Shared locator for compiled SGRN binaries (xmake build outputs + legacy dist/prefix fallbacks)."""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parents[1]


def _is_executable(path: Path) -> bool:
    return path.is_file() and (sys.platform == "win32" or os.access(path, os.X_OK))


def xmake_bin_dirs(root: Path = REPO_ROOT) -> list[Path]:
    """Return xmake binary output directories in preference order (release before debug)."""
    build_root = root / ".build"
    if sys.platform == "win32":
        ordered = [
            build_root / "windows" / "x64" / "release",
            build_root / "windows" / "x64" / "debug",
            build_root / "mingw" / "x86_64" / "release",
            build_root / "mingw" / "x86_64" / "debug",
        ]
    elif sys.platform == "darwin":
        ordered = [
            build_root / "macosx" / "arm64" / "release",
            build_root / "macosx" / "x86_64" / "release",
            build_root / "macosx" / "arm64" / "debug",
            build_root / "macosx" / "x86_64" / "debug",
        ]
    else:
        ordered = [
            build_root / "linux" / "x86_64" / "release",
            build_root / "linux" / "x86_64" / "debug",
        ]

    seen = set(ordered)
    if build_root.is_dir():
        for mode in ("release", "debug"):
            for d in sorted(build_root.glob(f"*/*/{mode}")):
                if d.is_dir() and d not in seen:
                    ordered.append(d)
                    seen.add(d)
    return ordered


def candidate_bin_dirs(name: str, root: Path = REPO_ROOT, extra_dirs: Iterable[Path] = ()) -> list[Path]:
    """Return all candidate directories for binary `name`, preferring fresh xmake builds."""
    app_name = {"sgrn_replay": "replay"}.get(name, name)
    dirs: list[Path] = list(extra_dirs)
    dirs.extend(xmake_bin_dirs(root))
    if sys.platform == "win32":
        dirs.extend([
            root / ".build/win64-native-static/sgrn/apps" / app_name,
            root / ".dist/win64-native-static",
            root / ".dist/win64-static",
            root / ".prefix/win64-static/bin",
        ])
    else:
        dirs.extend([
            root / ".dist/linux-static-release",
            root / ".dist/linux-static-release/bin",
            root / ".dist/linux-static",
            root / ".prefix/linux-static/bin",
            root / ".prefix/bin",
            root / ".build/linux-static-release",
            root / ".build/linux-static-release/bin",
            root / ".build/linux-static-release/sgrn/apps" / app_name,
            root / ".build/linux-static/sgrn/apps" / app_name,
            root / ".build/linux-static-release/sgrn" / app_name,
            root / ".build/linux-static/sgrn" / app_name,
        ])
    return dirs


def find_binary(
    name: str,
    root: Path = REPO_ROOT,
    extra_dirs: Iterable[Path] = (),
    check_path: bool = True,
) -> Path | None:
    """Locate a compiled SGRN binary, returning None if not found."""
    exts = [".exe", ""] if sys.platform == "win32" else ["", ".exe"]
    names = [f"{name}{ext}" for ext in exts]

    for directory in candidate_bin_dirs(name, root, extra_dirs):
        for n in names:
            cand = directory / n
            if _is_executable(cand):
                return cand

    if check_path:
        for n in names:
            found = shutil.which(n)
            if found:
                return Path(found)
    return None


def require_binary(
    name: str,
    root: Path = REPO_ROOT,
    extra_dirs: Iterable[Path] = (),
    check_path: bool = True,
) -> Path:
    """Locate a compiled SGRN binary or raise FileNotFoundError."""
    found = find_binary(name, root=root, extra_dirs=extra_dirs, check_path=check_path)
    if found is None:
        raise FileNotFoundError(f"Cannot find {name}; build the binaries first with `xmake -j10`.")
    return found
