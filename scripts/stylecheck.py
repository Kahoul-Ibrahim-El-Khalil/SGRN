#!/usr/bin/env python3
"""
stylecheck.py — libclang-based C++ naming convention checker & fixer.

This is Option 1 from the design doc: it parses real C++ using Clang's AST
(via libclang), so it understands pointers, shared_ptr/unique_ptr, class vs.
plain-struct, macros, etc. instead of guessing with regexes.

It is a STANDALONE tool, meant to run alongside clang-tidy in CI or a
pre-commit hook — it is not a clang-tidy plugin (clang-tidy's plugin API is
C++-only; libclang is the practical way to do this from Python).

USAGE
-----
    pip install --break-system-packages clang libclang

    # report violations only (safe, default)
    python3 stylecheck.py src/

    # also rewrite the files in place
    python3 stylecheck.py src/ --fix

    # use compile_commands.json for accurate include paths / flags
    python3 stylecheck.py src/ --compile-commands build/compile_commands.json

    # override the default naming rules
    python3 stylecheck.py src/ --config stylecheck.toml

IMPORTANT LIMITATIONS OF --fix
-------------------------------
- --fix renames variables, parameters, fields/members, and constants, and
  updates every reference to them that it can find *within the files you
  scan in this run*. It does NOT rename classes, functions/methods, or
  macros automatically — those have a much bigger blast radius (virtual
  overrides, ABI, macro-expansion, external callers) and are reported only,
  never rewritten.
- References that live in files outside the directory you pass in (e.g. a
  header used by another repo, or a build artifact) will not be updated.
- Run it on a clean git tree so you can review/revert the diff, and re-run
  your build + tests after --fix.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    from clang.cindex import (
        Config,
        CursorKind,
        Index,
        TokenKind,
        TranslationUnit,
        TypeKind,
        CompilationDatabase,
        CompilationDatabaseError,
    )
except ImportError:
    print(
        "error: the 'clang' python package is required.\n"
        "       pip install --break-system-packages clang libclang",
        file=sys.stderr,
    )
    sys.exit(1)

# --------------------------------------------------------------------------
# Default naming rules (from the design doc). Overridable via --config toml.
# --------------------------------------------------------------------------

DEFAULT_RULES = {
    "class": r"^[A-Z][A-Za-z0-9]*$",
    "function": r"^[a-z][A-Za-z0-9]*$",
    "local_var": r"^[a-z][a-z0-9_]*$",
    "param": r"^t_[a-z][a-z0-9_]*$",
    "raw_ptr_local": r"^p_[a-z][a-z0-9_]*$",
    "raw_ptr_param": r"^tp_[a-z][a-z0-9_]*$",
    "shared_ptr_local": r"^sp_[a-z][a-z0-9_]*$",
    "shared_ptr_param": r"^tsp_[a-z][a-z0-9_]*$",
    "unique_ptr_local": r"^up_[a-z][a-z0-9_]*$",
    "unique_ptr_param": r"^tup_[a-z][a-z0-9_]*$",
    "constant": r"^k[A-Z][A-Za-z0-9]*$",
    "macro": r"^[A-Z][A-Z0-9_]*$",
    "member": r"^[a-z][a-z0-9_]*_$",
    "plain_struct_field": r"^[a-z][a-z0-9_]*$",
}

SOURCE_EXTS = {".cpp", ".cc", ".cxx", ".hpp", ".h", ".hh", ".hxx"}
SKIP_DIR_NAMES = {".git", "build", "cmake-build-debug", "cmake-build-release", "third_party", "vendor", "orm"}

# Categories we are willing to auto-rename. Classes/functions/macros are
# report-only (see module docstring for why).
FIXABLE_CATEGORIES = {
    "local_var", "param", "raw_ptr_local", "raw_ptr_param",
    "shared_ptr_local", "shared_ptr_param", "unique_ptr_local",
    "unique_ptr_param", "constant", "member", "plain_struct_field",
}


def load_rules(config_path: str | None) -> dict:
    rules = dict(DEFAULT_RULES)
    if not config_path:
        return rules
    try:
        import tomllib
    except ImportError:
        print("warning: --config requires Python 3.11+ (tomllib); ignoring", file=sys.stderr)
        return rules
    with open(config_path, "rb") as fh:
        overrides = tomllib.load(fh)
    for key, pattern in overrides.get("rules", overrides).items():
        if key in rules:
            rules[key] = pattern
        else:
            print(f"warning: unknown rule key '{key}' in config, ignoring", file=sys.stderr)
    return rules


# --------------------------------------------------------------------------
# Data model
# --------------------------------------------------------------------------

@dataclass
class Violation:
    file: str
    line: int
    col: int
    category: str
    name: str
    expected_pattern: str
    kind_desc: str
    usr: str | None = None          # for cross-reference renaming
    offset: int | None = None       # start byte offset of the identifier token
    end_offset: int | None = None   # end byte offset of the identifier token
    fixable: bool = False


@dataclass
class Rename:
    """A single new-name proposal for one declaration (identified by USR)."""
    usr: str
    old_name: str
    new_name: str


def make_expected_name(name: str, category: str) -> str:
    """Given a bad name, propose a fixed one by applying the right prefix/suffix.

    This is best-effort: it strips any existing recognized prefix/suffix,
    converts to snake_case, then reapplies the correct convention.
    """
    # CamelCase / mixedCase -> snake_case, without splitting acronym runs
    # (e.g. "BufferSize" -> "Buffer_Size", but "MAX_size" stays "MAX_size")
    s = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name)
    s = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", "_", s)
    s = s.lower()
    s = re.sub(r"_+", "_", s).strip("_")

    prefix_map = {
        "param": "t_",
        "raw_ptr_local": "p_",
        "raw_ptr_param": "tp_",
        "shared_ptr_local": "sp_",
        "shared_ptr_param": "tsp_",
        "unique_ptr_local": "up_",
        "unique_ptr_param": "tup_",
    }
    known_prefixes = ("t_", "tp_", "tsp_", "tup_", "p_", "sp_", "up_", "k")
    for p in known_prefixes:
        if s.startswith(p):
            s = s[len(p):]
            break
    s = s.strip("_") or "value"

    if category in prefix_map:
        return prefix_map[category] + s
    if category == "constant":
        return "k" + "".join(w.capitalize() for w in s.split("_"))
    if category == "class":
        return "".join(w.capitalize() for w in s.split("_"))
    if category == "function":
        parts = s.split("_")
        return parts[0] + "".join(w.capitalize() for w in parts[1:])
    if category == "macro":
        return s.upper()
    if category == "member":
        return s + "_"
    # local_var, plain_struct_field
    return s


# --------------------------------------------------------------------------
# Clang helpers
# --------------------------------------------------------------------------

def find_libclang():
    """Best-effort auto-locate libclang.so if not already configured."""
    import ctypes.util
    candidates = [
        ctypes.util.find_library("clang"),
    ]
    try:
        import clang.native  # 'libclang' pip package ships this
        candidates.append(str(Path(clang.native.__file__).parent / "libclang.so"))
    except Exception:
        pass
    for c in candidates:
        if c:
            try:
                Config.set_library_file(c)
                return
            except Exception:
                continue


def type_pointer_kind(clang_type) -> str | None:
    """Return 'raw', 'shared', or 'unique' if the type is one of those, else None."""
    if clang_type.kind == TypeKind.POINTER:
        return "raw"
    spelling = clang_type.get_canonical().spelling
    if "shared_ptr" in spelling:
        return "shared"
    if "unique_ptr" in spelling:
        return "unique"
    return None


def identifier_extent(cursor):
    """Locate the (start_offset, end_offset) of the *identifier token* for a
    declaration cursor (not the whole declaration, e.g. not the type)."""
    name = cursor.spelling
    try:
        tokens = list(cursor.get_tokens())
    except Exception:
        tokens = []
    for tok in tokens:
        if tok.kind == TokenKind.IDENTIFIER and tok.spelling == name:
            return tok.extent.start.offset, tok.extent.end.offset
    ext = cursor.extent
    return ext.start.offset, ext.end.offset


def is_in_file(cursor, filepath: Path) -> bool:
    loc_file = cursor.location.file
    if loc_file is None:
        return False
    try:
        return Path(loc_file.name).resolve() == filepath.resolve()
    except OSError:
        return False


def struct_has_methods(cursor) -> bool:
    return any(c.kind == CursorKind.CXX_METHOD for c in cursor.get_children())


# --------------------------------------------------------------------------
# Core walk: collect violations (and, for renamed decls, their USR) for one TU
# --------------------------------------------------------------------------

def walk_tu(tu, filepath: Path, rules: dict) -> list[Violation]:
    violations: list[Violation] = []

    def check(category: str, cursor, name: str, kind_desc: str):
        pattern = rules[category]
        if re.match(pattern, name):
            return
        expected = make_expected_name(name, category)
        start, end = identifier_extent(cursor)
        violations.append(
            Violation(
                file=str(filepath),
                line=cursor.location.line,
                col=cursor.location.column,
                category=category,
                name=name,
                expected_pattern=expected,
                kind_desc=kind_desc,
                usr=cursor.get_usr(),
                offset=start,
                end_offset=end,
                fixable=category in FIXABLE_CATEGORIES,
            )
        )

    def visit(cursor):
        if not is_in_file(cursor, filepath):
            # still recurse in case of nested same-file cursors inside macro
            # expansions from headers; cheap guard, doesn't hurt correctness
            for c in cursor.get_children():
                visit(c)
            return

        kind = cursor.kind
        name = cursor.spelling

        if kind in (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL) and cursor.is_definition() and name:
            check("class", cursor, name, f"{'Class' if kind == CursorKind.CLASS_DECL else 'Struct'} '{name}'")

        elif kind in (CursorKind.FUNCTION_DECL, CursorKind.CXX_METHOD) and name:
            if not (name.startswith("operator") or name.startswith("~")):
                check("function", cursor, name, f"Function '{name}'")

        elif kind == CursorKind.PARM_DECL and name:
            pk = type_pointer_kind(cursor.type)
            category = {
                "raw": "raw_ptr_param",
                "shared": "shared_ptr_param",
                "unique": "unique_ptr_param",
            }.get(pk, "param")
            check(category, cursor, name, f"Parameter '{name}'")

        elif kind == CursorKind.VAR_DECL and name:
            semantic_parent_kind = cursor.semantic_parent.kind if cursor.semantic_parent else None
            is_global_scope = semantic_parent_kind in (
                CursorKind.TRANSLATION_UNIT, CursorKind.NAMESPACE
            )
            is_const = cursor.type.is_const_qualified()
            if is_const and (is_global_scope or cursor.storage_class.name == "STATIC"):
                check("constant", cursor, name, f"Constant '{name}'")
            else:
                pk = type_pointer_kind(cursor.type)
                category = {
                    "raw": "raw_ptr_local",
                    "shared": "shared_ptr_local",
                    "unique": "unique_ptr_local",
                }.get(pk, "local_var")
                check(category, cursor, name, f"Variable '{name}'")

        elif kind == CursorKind.FIELD_DECL and name:
            parent = cursor.semantic_parent
            if parent is not None and parent.kind == CursorKind.STRUCT_DECL and not struct_has_methods(parent):
                check("plain_struct_field", cursor, name, f"Plain struct field '{name}'")
            else:
                check("member", cursor, name, f"Class member '{name}'")

        elif kind == CursorKind.MACRO_DEFINITION and name:
            check("macro", cursor, name, f"Macro '{name}'")

        for c in cursor.get_children():
            visit(c)

    visit(tu.cursor)
    return violations


def find_references(tu, filepath: Path, target_usrs: set[str]) -> list[tuple[str, int, int]]:
    """Find (usr, start_offset, end_offset) for every reference-like cursor in
    this TU (within `filepath`) whose referenced declaration has a USR in
    `target_usrs`. Used to update call sites after renaming a declaration."""
    hits: list[tuple[str, int, int]] = []
    REF_KINDS = {
        CursorKind.DECL_REF_EXPR,
        CursorKind.MEMBER_REF_EXPR,
        CursorKind.MEMBER_REF,
    }

    def visit(cursor):
        if not is_in_file(cursor, filepath):
            for c in cursor.get_children():
                visit(c)
            return
        if cursor.kind in REF_KINDS:
            ref = cursor.referenced
            if ref is not None:
                usr = ref.get_usr()
                if usr in target_usrs:
                    start, end = identifier_extent(cursor)
                    hits.append((usr, start, end))
        for c in cursor.get_children():
            visit(c)

    visit(tu.cursor)
    return hits


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def collect_files(root: Path, exts: set[str]) -> list[Path]:
    files = []
    for p in root.rglob("*"):
        if p.is_dir():
            continue
        if any(part in SKIP_DIR_NAMES for part in p.parts):
            continue
        if p.suffix in exts:
            files.append(p)
    return sorted(files)


_SYSTEM_INCLUDE_ARGS: list[str] | None = None
ENV_MICROMAMBA = "SGRN"

def system_include_args() -> list[str]:
    """Best-effort discovery of the system C++ include search paths, via the
    installed g++/clang++ driver. Without this, environments that only have
    libclang.so (no full compiler driver) can fail to fully resolve standard
    headers like <memory>, which breaks shared_ptr/unique_ptr detection.
    Cached; falls back to no extra args if no compiler driver is found."""
    global _SYSTEM_INCLUDE_ARGS
    if _SYSTEM_INCLUDE_ARGS is not None:
        return _SYSTEM_INCLUDE_ARGS
    import subprocess
    import os
    args: list[str] = []
    
    compilers = []
    if "CXX" in os.environ:
        compilers.append(os.environ["CXX"])
    
    if ENV_MICROMAMBA:
        home = os.path.expanduser("~")
        compilers.append(os.path.join(home, "micromamba", "envs", ENV_MICROMAMBA, "bin", "clang++"))
        
    compilers.extend(["clang++", "g++"])
    
    for compiler in compilers:
        try:
            result = subprocess.run(
                [compiler, "-E", "-x", "c++", "-", "-v"],
                input="", capture_output=True, text=True, timeout=5,
            )
        except (FileNotFoundError, subprocess.SubprocessError):
            continue
        stderr = result.stderr
        if "#include <...> search starts here" not in stderr:
            continue
        in_block = False
        for line in stderr.splitlines():
            if "#include <...> search starts here" in line:
                in_block = True
                continue
            if "End of search list" in line:
                break
            if in_block:
                path = line.strip().split(" ")[0]
                if path:
                    args.append("-isystem")
                    args.append(path)
        if args:
            break
    _SYSTEM_INCLUDE_ARGS = args
    return args


_FALLBACK_ARGS = None

def get_compile_args(compile_commands: str | None, filepath: Path, target: str | None = None, sysroot: str | None = None) -> list[str]:
    global _FALLBACK_ARGS
    
    def apply_cross_args(base_args: list[str]) -> list[str]:
        res = list(base_args)
        if target:
            res.append(f"--target={target}")
        if sysroot:
            res.append(f"--sysroot={sysroot}")
            res.extend(["-nostdinc", "-nostdinc++"])
        return res
        
    if compile_commands:
        try:
            db = CompilationDatabase.fromDirectory(str(Path(compile_commands).parent))
        except CompilationDatabaseError:
            return apply_cross_args(["-std=c++17"] + system_include_args())

        try:
            cmds = db.getCompileCommands(str(filepath))
            if cmds:
                raw_args = list(cmds[0].arguments)[1:]  # drop compiler executable
                filtered_args = []
                i = 0
                while i < len(raw_args):
                    arg = raw_args[i]
                    if arg == "-c":
                        i += 1
                        continue
                    if arg == "-o":
                        i += 2
                        continue
                    if arg == "-Winvalid-pch":
                        i += 1
                        continue
                    if arg == "-Xclang" and i + 1 < len(raw_args) and (raw_args[i+1] == "-include-pch" or raw_args[i+1] == "-include"):
                        i += 4  # skip -Xclang, -include[-pch], -Xclang, <pch_file>
                        continue
                    if arg == "--":
                        break
                    if str(filepath) in arg or arg.endswith(".cpp") or arg.endswith(".cc") or arg.endswith(".c") or arg.endswith(".hpp"):
                        i += 1
                        continue
                    filtered_args.append(arg)
                    i += 1
                return apply_cross_args(filtered_args)
        except CompilationDatabaseError:
            pass
            
        # If we reach here, no commands were found for this file (likely a header).
        # Build a fallback list of includes/defines by scanning the DB once.
        if _FALLBACK_ARGS is None:
            _FALLBACK_ARGS = []
            try:
                all_cmds = db.getAllCompileCommands()
                if all_cmds:
                    seen = set()
                    for cmd in all_cmds:
                        args = list(cmd.arguments)
                        for i, arg in enumerate(args):
                            if arg.startswith("-I") or arg.startswith("-D") or arg.startswith("-isystem"):
                                if arg not in seen:
                                    seen.add(arg)
                                    _FALLBACK_ARGS.append(arg)
                                    if arg == "-isystem" and i + 1 < len(args):
                                        seen.add(args[i+1])
                                        _FALLBACK_ARGS.append(args[i+1])
            except CompilationDatabaseError:
                pass
        if _FALLBACK_ARGS is not None:
            ret = apply_cross_args(["-std=c++17"] + _FALLBACK_ARGS + system_include_args())
            print("Using fallback args:", ret)
            return ret
            
    return apply_cross_args(["-std=c++17"] + system_include_args())


def print_report(violations: list[Violation]):
    if not violations:
        print("No naming convention violations found.")
        return
    for v in violations:
        print(f"{v.file}:{v.line}:{v.col}")
        print(f"  {v.kind_desc}")
        print(f"  Expected: {v.expected_pattern}"
              f"{'' if v.fixable else '  [report-only: rerun with --fix does not touch this category]'}")
        print()
    fixable_count = sum(1 for v in violations if v.fixable)
    print(f"{len(violations)} violation(s) found ({fixable_count} auto-fixable with --fix).")


def apply_fixes(all_violations: list[Violation], all_refs: dict[Path, list[tuple[str, int, int]]]):
    """Rewrite files in place, renaming fixable violations and all discovered
    references to them."""
    renames: dict[str, str] = {}
    for v in all_violations:
        if v.fixable and v.usr:
            renames[v.usr] = v.expected_pattern

    # group all edits (decl + refs) per file
    edits_by_file: dict[str, list[tuple[int, int, str]]] = {}

    for v in all_violations:
        if v.fixable and v.offset is not None:
            edits_by_file.setdefault(v.file, []).append(
                (v.offset, v.end_offset, v.expected_pattern)
            )

    for filepath, refs in all_refs.items():
        for usr, start, end in refs:
            if usr in renames:
                edits_by_file.setdefault(str(filepath), []).append(
                    (start, end, renames[usr])
                )

    import concurrent.futures

    changed_files = 0
    for file_str, edits in edits_by_file.items():
        path = Path(file_str)
        # CRITICAL: Read as bytes because Clang offsets are BYTE offsets!
        text_bytes = path.read_bytes()
        # apply from the end of the file backwards so earlier offsets stay valid
        edits.sort(key=lambda e: e[0], reverse=True)
        seen_spans = set()
        for start, end, new_name in edits:
            span = (start, end)
            if span in seen_spans:
                continue
            seen_spans.add(span)
            new_name_bytes = new_name.encode("utf-8")
            text_bytes = text_bytes[:start] + new_name_bytes + text_bytes[end:]
        path.write_bytes(text_bytes)
        print(f"Fixed {file_str}")
        changed_files += 1
    print(f"Rewrote {changed_files} file(s) total.")


def process_file_worker(args_tuple):
    """Worker function for multiprocessing."""
    i, total, f, rules, compile_commands, target, sysroot = args_tuple
    find_libclang()
    print(f"[{i}/{total}] Parsing {f}...", flush=True)
    compile_args = get_compile_args(compile_commands, f, target, sysroot)
    
    # We must instantiate the Index per process
    index = Index.create()
    try:
        tu = index.parse(
            str(f),
            args=compile_args,
            options=TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD | TranslationUnit.PARSE_PRECOMPILED_PREAMBLE,
        )
    except Exception as e:
        print(f"warning: failed to parse {f}: {e}", file=sys.stderr)
        return f, []
        
    raw_errors = [d for d in tu.diagnostics if d.severity >= d.Error]
    real_errors = []
    for d in raw_errors:
        msg = str(d)
        if "limits.h" in msg or "stddef.h" in msg:
            continue
        real_errors.append(d)
        
    if real_errors:
        print(f"warning: {f} had parse errors (results may be incomplete):", file=sys.stderr)
        for d in real_errors[:3]:
            print(f"    {d}", file=sys.stderr)

    violations = walk_tu(tu, f, rules)
    return f, violations

def find_references_worker(args_tuple):
    i, total, f, target_usrs, target_names, compile_commands, target, sysroot = args_tuple
    find_libclang()
    
    # Fast path text check
    try:
        text = f.read_text(encoding="utf-8", errors="ignore")
        if not any(name in text for name in target_names):
            return f, []
    except Exception:
        pass

    print(f"[{i}/{total}] Finding references in {f}...", flush=True)
    compile_args = get_compile_args(compile_commands, f, target, sysroot)
    index = Index.create()
    try:
        tu = index.parse(
            str(f),
            args=compile_args,
            options=TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD | TranslationUnit.PARSE_PRECOMPILED_PREAMBLE,
        )
    except Exception:
        return f, []
    
    refs = find_references(tu, f, target_usrs)
    return f, refs


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", help="File or directory to check")
    parser.add_argument("--fix", action="store_true", help="Rewrite files in place")
    parser.add_argument("--compile-commands", help="Path to compile_commands.json")
    parser.add_argument("--config", help="Path to a TOML file overriding naming rules")
    parser.add_argument("--extensions", nargs="+", default=sorted(SOURCE_EXTS),
                         help="File extensions to scan (default: common C/C++ ones)")
    parser.add_argument("-j", "--jobs", type=int, default=None, help="Number of parallel jobs")
    parser.add_argument("--target", default=None, help="Optional target triple for cross-compilation")
    parser.add_argument("--sysroot", default=None, help="Optional sysroot path for cross-compilation")
    args = parser.parse_args()

    find_libclang()
    rules = load_rules(args.config)

    root = Path(args.path)
    exts = {e if e.startswith(".") else f".{e}" for e in args.extensions}
    files = [root] if root.is_file() else collect_files(root, exts)

    if not files:
        print(f"No source files found under {root}", file=sys.stderr)
        sys.exit(1)

    all_violations: list[Violation] = []
    all_refs: dict[Path, list[tuple[str, int, int]]] = {}
    
    # We must run single-threaded if we want to extract references safely 
    # because `tu` objects cannot easily be pickled back from a ProcessPool.
    # But now we don't return `tu`, so we can use ProcessPoolExecutor!
    import concurrent.futures
    
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as executor:
        futures = []
        for i, f in enumerate(files, 1):
            futures.append(executor.submit(process_file_worker, (i, len(files), f, rules, args.compile_commands, args.target, args.sysroot)))
            
        for future in concurrent.futures.as_completed(futures):
            f, violations = future.result()
            all_violations.extend(violations)

    print_report(all_violations)

    if args.fix:
        fixable_usrs = {v.usr for v in all_violations if v.fixable and v.usr}
        fixable_names = {v.name for v in all_violations if v.fixable and v.usr}
        if fixable_usrs:
            with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as executor:
                futures = []
                for i, f in enumerate(files, 1):
                    futures.append(executor.submit(find_references_worker, (i, len(files), f, fixable_usrs, fixable_names, args.compile_commands, args.target, args.sysroot)))
                
                for future in concurrent.futures.as_completed(futures):
                    f, refs = future.result()
                    if refs:
                        all_refs[f] = refs
            apply_fixes(all_violations, all_refs)
        else:
            print("Nothing auto-fixable.")


if __name__ == "__main__":
    main()
