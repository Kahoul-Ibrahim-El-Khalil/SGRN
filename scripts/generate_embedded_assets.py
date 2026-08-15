#!/usr/bin/env python3
# =============================================================================
# generate_embedded_assets.py
# =============================================================================
# Generic SGRN Embedded Virtual File System (VFS) asset generator.
#
# Replaces the web-only generate_web_headers.py for any file category:
#   web      — HTML, JS, CSS, images   → served by Drogon HTTP handlers
#   sql      — *.sql migration scripts  → executed at init / migration time
#   config   — *.json / *.toml defaults → dumped to disk on `init`
#   cert     — TLS material             → loaded into memory at startup
#   other    — anything else
#
# Generated namespace convention:
#   sgrn::{project}::assets::{kind}
#
# Examples:
#   sgrn::datastore::assets::web
#   sgrn::datastore::assets::sql
#   sgrn::gateway::assets::web
#   python3 generate_embedded_assets.py <src_dir> <out_dir>          \
#       --namespace sgrn::datastore::vfs::web                        \
#       --kind web                                                    \
#       --extensions .html .js .css .png .svg .ico .jpg              \
#       --level 19
#
#   python3 generate_embedded_assets.py <src_dir> <out_dir>          \
#       --namespace sgrn::datastore::vfs::sql                        \
#       --kind sql                                                    \
#       --extensions .sql                                             \
#       --virtual-prefix /sql
# =============================================================================
import argparse
import re
import sys
from pathlib import Path

import zstandard as zstd

# ---------------------------------------------------------------------------
# Extension → MIME type table (used for web assets only)
# ---------------------------------------------------------------------------
MIME_MAP = {
    ".html": "text/html; charset=utf-8",
    ".css":  "text/css; charset=utf-8",
    ".js":   "application/javascript; charset=utf-8",
    ".mjs":  "application/javascript; charset=utf-8",
    ".svg":  "image/svg+xml",
    ".png":  "image/png",
    ".jpg":  "image/jpeg",
    ".jpeg": "image/jpeg",
    ".ico":  "image/x-icon",
    ".webp": "image/webp",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
    ".json": "application/json",
    ".txt":  "text/plain; charset=utf-8",
    ".sql":  "text/plain; charset=utf-8",
    ".toml": "text/plain; charset=utf-8",
}

KIND_MAP = {
    "web":    "sgrn::AssetKind::Web",
    "sql":    "sgrn::AssetKind::Sql",
    "config": "sgrn::AssetKind::Config",
    "cert":   "sgrn::AssetKind::Cert",
    "other":  "sgrn::AssetKind::Other",
}


def makeIndentifier(rel_path: Path) -> str:
    """Convert a relative path to a SCREAMING_SNAKE_CASE C++ identifier."""
    name = re.sub(r"[^A-Za-z0-9]", "_", str(rel_path))
    name = re.sub(r"_+", "_", name)
    return name.strip("_").upper()


def bytesToCppArray(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk))
    return ",\n".join(lines)


def stripSqlComments(text: str) -> str:
    """Remove SQL -- line comments and /* */ block comments, then collapse blank lines."""
    import re
    # Remove /* ... */ block comments (non-greedy, DOTALL)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    # Remove -- line comments (but NOT inside string literals — good-enough heuristic)
    text = re.sub(r"--[^\n]*", "", text)
    # Collapse runs of blank lines into a single blank line
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def stripConfigComments(text: str) -> str:
    """Remove # comment lines and trailing # comments from config/service files."""
    import re
    lines = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):          # full-line comment → drop
            continue
        # Trailing inline comment: remove only when preceded by whitespace
        line = re.sub(r"\s+#[^\n]*", "", line)
        lines.append(line)
    # Collapse runs of blank lines
    result = re.sub(r"\n{3,}", "\n\n", "\n".join(lines))
    return result.strip()


def flattenSql(path: Path) -> bytes:
    """Reads a SQL file, strips comments and psql meta-commands, inlines \\i includes recursively."""
    output = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except Exception as e:
        print(f"Error reading {path}: {e}")
        return b""
    for line in lines:
        line_stripped = line.strip()

        if line_stripped.startswith(r"\i ") or line_stripped.startswith(r"\ir "):
            include_rel_path = line_stripped.split(" ", 1)[1].strip(" '\"")
            include_abs_path = path.parent / include_rel_path
            output.append(f"-- INLINED: {include_rel_path}")
            output.append(flattenSql(include_abs_path).decode("utf-8"))
        else:
            output.append(line)
    raw_text = "\n".join(output)
    return stripSqlComments(raw_text).encode("utf-8")


def compress_file(path: Path, level: int, kind: str) -> tuple[bytes, int]:
    if kind == "sql":
        raw = flattenSql(path)
    elif kind == "config" and path.suffix.lower() in {".conf", ".service", ".ini"}:
        raw_text = path.read_text(encoding="utf-8")
        raw = stripConfigComments(raw_text).encode("utf-8")
    else:
        raw = path.read_bytes()
    compressed = zstd.ZstdCompressor(level=level).compress(raw)
    print(f"  Compressing {path.name}: {len(raw):,} → {len(compressed):,} bytes")
    return compressed, len(raw)


def generateAssetHeader(
    file_path: Path,
    out_dir: Path,
    level: int,
    namespace: str,
    rel_path: Path,
    kind: str,
) -> tuple[str, str]:
    """
    Compress one file and write its individual C++ header.
    Returns (symbol_prefix, header_filename_stem).
    """
    compressed, original_size = compress_file(file_path, level, kind)
    symbol = makeIndentifier(rel_path)
    header_stem = str(rel_path).replace("/", "_").replace("\\", "_")
    header_path = out_dir / f"{header_stem}.hpp"

    with open(header_path, "w") as f:
        f.write(f"// Auto-generated — do not edit. Source: {rel_path}\n")
        f.write("// Payload compressed with Zstd. High entropy by design (IP protection).\n")
        f.write("#pragma once\n")
        f.write("#include <cstdint>\n")
        f.write("#include <cstddef>\n\n")
        f.write(f"namespace {namespace} {{\n\n")
        f.write(f"// .rodata — Zstd-compressed payload for {rel_path}\n")
        f.write(f"inline constexpr uint8_t {symbol}_DATA[] = {{\n")
        f.write(bytesToCppArray(compressed))
        f.write("\n};\n\n")
        f.write(f"inline constexpr size_t {symbol}_COMPRESSED_SIZE = sizeof({symbol}_DATA);\n")
        f.write(f"inline constexpr size_t {symbol}_ORIGINAL_SIZE   = {original_size};\n\n")
        f.write(f"}} // namespace {namespace}\n")

    return symbol, header_stem


def generateMasterHeader(
    out_dir: Path,
    namespace: str,
    kind: str,
    virtual_prefix: str,
    assets: list[tuple[str, str, str, str]],  # (symbol, header_stem, virtual_path, mime)
    master_name: str,
) -> None:
    """
    Write the master aggregating header that exposes the asset table
    and a sgrn::vfs::Registry<N> instance.
    """
    kind_cpp = KIND_MAP[kind]
    master_path = out_dir / master_name

    with open(master_path, "w") as f:
        f.write("// Auto-generated master asset header — do not edit.\n")
        f.write(f"// Namespace : {namespace}\n")
        f.write(f"// Kind      : {kind}\n")
        f.write("#pragma once\n")
        f.write("#include <sgrn/assets/EmbeddedAsset.hpp>\n\n")
        for _, header_stem, _, _ in assets:
            f.write(f'#include "{header_stem}.hpp"\n')
        f.write(f"\nnamespace {namespace} {{\n\n")

        # Asset array — lives in .rodata
        f.write("// .rodata — asset descriptor table\n")
        f.write("inline constexpr sgrn::EmbeddedAsset ASSETS[] = {\n")
        for symbol, _, virtual_path, mime in assets:
            vpath = f"{virtual_prefix}/{virtual_path}".replace("//", "/")
            f.write(f'    {{"{vpath}", "{mime}",\n')
            f.write(f"      {symbol}_DATA, {symbol}_COMPRESSED_SIZE, {symbol}_ORIGINAL_SIZE,\n")
            f.write(f"      {kind_cpp}}},\n")
        f.write("};\n\n")

        # Registry — also in .rodata (constexpr)
        f.write("// Compile-time sorted registry — O(log N) lookup, zero heap allocation.\n")
        f.write("inline constexpr auto VFS = sgrn::AssetRegistry(ASSETS);\n\n")
        f.write(f"inline constexpr size_t ASSET_COUNT = VFS.size();\n\n")
        f.write(f"}} // namespace {namespace}\n")

    print(f"\nGenerated master header: {master_path}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate Zstd-compressed C++ VFS headers for any SGRN asset category."
    )
    parser.add_argument("src_dir",  help="Source directory to scan for assets")
    parser.add_argument("out_dir",  help="Output directory for generated C++ headers")
    parser.add_argument("--namespace",      default="sgrn::datastore::assets::web",
                        help="C++ namespace (convention: sgrn::{project}::assets::{kind})")
    parser.add_argument("--kind",           default="web",
                        choices=["web", "sql", "config", "cert", "other"],
                        help="Semantic kind of all assets in this batch")
    parser.add_argument("--extensions",     nargs="+",
                        help="File extensions to include (e.g. .html .js .css). "
                             "Defaults to a kind-appropriate set.")
    parser.add_argument("--virtual-prefix", default="",
                        help="Prefix prepended to every virtual path (e.g. /sql)")
    parser.add_argument("--master",         default="assets.hpp",
                        help="Name of the master aggregating header file")
    parser.add_argument("--level",          type=int, default=19,
                        help="Zstd compression level (1–22, default 19)")
    parser.add_argument("--exclude",        nargs="+", default=[],
                        metavar="PATTERN",
                        help="Filenames or relative path substrings to exclude (e.g. compile_commands.json)")
    args = parser.parse_args()

    # Default extension sets per kind
    default_extensions = {
        "web":    {".html", ".js", ".mjs", ".css", ".svg", ".png",
                   ".jpg", ".jpeg", ".ico", ".webp", ".woff", ".woff2"},
        "sql":    {".sql"},
        "config": {".conf", ".service"},
        "cert":   {".pem", ".crt", ".key", ".cer"},
        "other":  None,  # None means: include everything
    }

    allowed_exts = (
        {e if e.startswith(".") else f".{e}" for e in args.extensions}
        if args.extensions
        else default_extensions[args.kind]
    )

    src_dir = Path(args.src_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not src_dir.is_dir():
        print(f"Error: source directory '{src_dir}' does not exist.", file=sys.stderr)
        sys.exit(1)

    assets: list[tuple[str, str, str, str]] = []

    for f in sorted(src_dir.rglob("*")):
        if not f.is_file():
            continue
        if allowed_exts is not None and f.suffix.lower() not in allowed_exts:
            continue

        # For SQL assets, only process the root 'init.sql' because it recursively inlines all other files.
        if args.kind == "sql":
            if f.name != "init.sql" or f.parent != src_dir:
                continue

        # Apply --exclude patterns (match against filename or any path component)
        rel_str = f.relative_to(src_dir).as_posix()
        if any(pat in rel_str or pat == f.name for pat in args.exclude):
            print(f"  Excluding: {rel_str}")
            continue

        rel_path = f.relative_to(src_dir)
        symbol, header_stem = generateAssetHeader(
            f, out_dir, args.level, args.namespace, rel_path, args.kind
        )
        virtual_path = rel_path.as_posix()
        mime = MIME_MAP.get(f.suffix.lower(), "application/octet-stream")
        assets.append((symbol, header_stem, virtual_path, mime))

    if not assets:
        print("Warning: no matching files found.", file=sys.stderr)
        sys.exit(0)

    generateMasterHeader(
        out_dir,
        args.namespace,
        args.kind,
        args.virtual_prefix,
        assets,
        args.master,
    )


if __name__ == "__main__":
    main()
