#!/usr/bin/env python3
# generate_views.py — compile-time CRUD view generator (manual step).
#
# Connects to a live (dev/migrated) Postgres instance (same connection
# pattern as generate_orm.py), introspects columns/PK/FK, emits one
# CrudViewSpec-based handler header per manifest-listed table, and appends
# (never removes) one registration line per newly-seen table.
#
# NOT wired into the CMake build as a live-DB dependency — run manually
# after schema changes, exactly like generate_orm.py is today; output is
# committed like the ORM models are.
#
# Intent lives in crud/manifest.json (the allow-list: which tables are
# exposed, which operations each table serves, which Drogon filters guard
# each operation, and per-column filter/write flags). The live database is
# consulted only for structural facts that must never drift from reality:
# column existence, udt_name -> FieldType mapping, and the primary key.
import argparse
import json
import os
import re
import sys
from pathlib import Path

# sgrn/lib/datastore — derived from this script's location so the filter
# scan and default paths work regardless of the caller's CWD (invoke from
# the repo root, e.g. `python3 sgrn/lib/datastore/scripts/generators/...`).
DATASTORE_ROOT = Path(__file__).resolve().parent.parent.parent

DEFAULT_MANIFEST = str(DATASTORE_ROOT / "crud" / "manifest.json")

TENANT_COLUMN_CANDIDATES = {"organisation", "organisation_id"}

# Columns that are never part of the API surface — not readable via the
# whitelist SELECT list, not filterable, not writable — even though they
# exist on the table. The engine's explicit column list (pk + tenant +
# fields) guarantees they are never returned. Listing one of these in the
# manifest is a hard error, so a manifest edit can never accidentally
# expose a password hash.
SENSITIVE_COLUMNS = {"password", "token_secret_hash", "password_hash", "secret"}

GEN_DIR = str(DATASTORE_ROOT / "src" / "handlers" / "generated")
REGISTRATION_FILE = f"{GEN_DIR}/RegisteredViews.cpp"

# Canonical CRUD operations, in route-table order (collection routes first,
# then item routes). A manifest "operations" list is always rendered in
# this order so regen is diff-stable regardless of list ordering.
ALL_OPERATIONS = ("list", "get", "create", "update", "delete")
COLLECTION_OPS = ("list", "create")
ITEM_OPS = ("get", "update", "delete")

OP_HANDLERS = {
    "list": "handleList",
    "get": "handleGet",
    "create": "handleCreate",
    "update": "handleUpdate",
    "delete": "handleDelete",
}

OP_METHODS = {
    "list": "drogon::Get",
    "get": "drogon::Get",
    "create": "drogon::Post",
    "update": "drogon::Patch",
    "delete": "drogon::Delete",
}

ALL_FILTER_OPS = ("eq", "neq", "gt", "gte", "lt", "lte", "like", "in")

USER_AUTH_FILTER = "sgrn::datastore::filters::UserAuthFilter"

# Dev-database credential defaults — kept identical to generate_orm.py's
# DB_CONFIG_TEMPLATE so both scripts "just work" against a default dev DB.
# Practically always overridden via --password (or PGPASSWORD) for real use.
DEFAULT_PASSWORD = "dracaeris"

TOP_LEVEL_KEYS = {"$schema_version", "default_filters", "schemas"}
SCHEMA_KEYS = {"tables"}
TABLE_KEYS = {
    "tenant_column",
    "read_relation",
    "write_table",
    "default_order",
    "max_limit",
    "operations",
    "operation_filters",
    "columns",
}
COLUMN_KEYS = {"filter_ops", "insertable", "updatable"}

SCHEMA_VERSION = 1

PG_TYPE_MAP = {
    "int2": "Int",
    "int4": "Int",
    "int8": "BigInt",
    "text": "Text",
    "varchar": "Text",
    "bpchar": "Text",
    "uuid": "Text",
    "inet": "Text",
    "numeric": "Text",
    "bool": "Bool",
    "timestamptz": "Timestamp",
    "timestamp": "Timestamp",
    "date": "Timestamp",
    "jsonb": "Jsonb",
    "json": "Jsonb",
}


def fail(msg):
    raise SystemExit(f"generate_views.py: error: {msg}")


# --- manifest loading & validation -------------------------------------

CLASS_RE = re.compile(r"class\s+(\w+)\s*:\s*public\s+drogon::HttpFilter\s*<")
NAMESPACE_RE = re.compile(r"namespace\s+([\w:]+)")


def filter_source_dirs():
    """Directories whose `class XFilter : public drogon::HttpFilter<`
    declarations form the KNOWN_FILTERS allow-list."""
    dirs = []
    candidate = DATASTORE_ROOT / "src" / "filters"
    if candidate.is_dir():
        dirs.append(candidate)
    include = DATASTORE_ROOT / "include"
    if include.is_dir():
        dirs.extend(sorted(p for p in include.rglob("filters") if p.is_dir()))
    return dirs


def scan_known_filters():
    """Build the allow-list of filter classes the manifest may reference.

    Filters are defined in C++ source; the manifest only references them by
    fully-qualified name. Anything not found by this scan is rejected.
    """
    known = {}
    dirs = filter_source_dirs()
    if not dirs:
        fail(f"no filter source directories found under {DATASTORE_ROOT} (looked for src/filters and include/**/filters)")
    for root in dirs:
        for path in sorted(root.rglob("*")):
            if path.suffix not in (".hpp", ".h", ".hxx", ".cpp"):
                continue
            try:
                text = path.read_text()
            except OSError:
                continue
            ns_match = NAMESPACE_RE.search(text)
            ns = ns_match.group(1) if ns_match else ""
            for class_match in CLASS_RE.finditer(text):
                cls = class_match.group(1)
                fq = f"{ns}::{cls}" if ns else cls
                known.setdefault(fq, str(path))
    if not known:
        fail(f"filter scan found no `class *Filter : public drogon::HttpFilter<` declarations under {[str(d) for d in dirs]}")
    return known


def check_filters(filters, known_filters, path):
    if not isinstance(filters, list):
        fail(f"{path}: expected a list of filter names, got {type(filters).__name__}")
    for i, name in enumerate(filters):
        if not isinstance(name, str):
            fail(f"{path}[{i}]: expected a filter name string, got {type(name).__name__}")
        if name not in known_filters:
            fail(
                f"{path}[{i}]: unknown filter '{name}' — no `class {name.split('::')[-1]} : "
                f"public drogon::HttpFilter<...>` declaration found under src/filters/ or include/**/filters/"
            )


def load_manifest(manifest_path, known_filters):
    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
    except FileNotFoundError:
        fail(f"manifest not found: {manifest_path}")
    except json.JSONDecodeError as e:
        fail(f"manifest {manifest_path}: invalid JSON: {e}")
    return validate_manifest(manifest, manifest_path, known_filters)


def validate_manifest(manifest, manifest_path, known_filters):
    if not isinstance(manifest, dict):
        fail(f"{manifest_path}: top level must be an object")
    for key in manifest:
        if key not in TOP_LEVEL_KEYS:
            fail(f"{manifest_path}: unknown top-level key '{key}' (allowed: {sorted(TOP_LEVEL_KEYS)})")
    if manifest.get("$schema_version") != SCHEMA_VERSION:
        fail(f"{manifest_path}: '$schema_version' must be {SCHEMA_VERSION}")

    default_filters = manifest.get("default_filters", {})
    if not isinstance(default_filters, dict):
        fail(f"{manifest_path}: 'default_filters' must be an object keyed by operation")
    for op, filters in default_filters.items():
        if op not in ALL_OPERATIONS:
            fail(f"{manifest_path}: default_filters: unknown operation '{op}' (allowed: {list(ALL_OPERATIONS)})")
        check_filters(filters, known_filters, f"{manifest_path}: default_filters.{op}")
    # Operations without an explicit default fall back to UserAuthFilter —
    # today's universal behavior — so minimal manifests stay concise.
    resolved_defaults = {}
    for op in ALL_OPERATIONS:
        if op in default_filters:
            resolved_defaults[op] = default_filters[op]
        elif USER_AUTH_FILTER in known_filters:
            resolved_defaults[op] = [USER_AUTH_FILTER]
        else:
            fail(f"{manifest_path}: default_filters: no entry for operation '{op}' and no {USER_AUTH_FILTER} found in source")

    schemas = manifest.get("schemas")
    if not isinstance(schemas, dict):
        fail(f"{manifest_path}: 'schemas' must be an object mapping schema name to {{'tables': ...}}")
    for schema, schema_cfg in schemas.items():
        s_path = f"{manifest_path}: schemas.{schema}"
        if not isinstance(schema_cfg, dict):
            fail(f"{s_path}: expected an object")
        for key in schema_cfg:
            if key not in SCHEMA_KEYS:
                fail(f"{s_path}: unknown key '{key}' (allowed: {sorted(SCHEMA_KEYS)})")
        tables = schema_cfg.get("tables", {})
        if not isinstance(tables, dict):
            fail(f"{s_path}.tables: expected an object mapping table name to config")
        for table, table_cfg in tables.items():
            validate_table(schema, table, table_cfg, manifest_path, known_filters)

    return manifest, resolved_defaults


def validate_table(schema, table, table_cfg, manifest_path, known_filters):
    t_path = f"{manifest_path}: schemas.{schema}.tables.{table}"
    if not isinstance(table_cfg, dict):
        fail(f"{t_path}: expected an object")
    for key in table_cfg:
        if key not in TABLE_KEYS:
            fail(f"{t_path}: unknown key '{key}' (allowed: {sorted(TABLE_KEYS)})")

    operations = table_cfg.get("operations", list(ALL_OPERATIONS))
    if not isinstance(operations, list):
        fail(f"{t_path}.operations: expected a list of operations, got {type(operations).__name__}")
    for i, op in enumerate(operations):
        if op not in ALL_OPERATIONS:
            fail(f"{t_path}.operations[{i}]: unknown operation '{op}' (allowed: {list(ALL_OPERATIONS)})")
    if len(set(operations)) != len(operations):
        fail(f"{t_path}.operations: duplicate operation entries")

    op_filters = table_cfg.get("operation_filters", {})
    if not isinstance(op_filters, dict):
        fail(f"{t_path}.operation_filters: expected an object keyed by operation")
    for op, filters in op_filters.items():
        if op not in ALL_OPERATIONS:
            fail(f"{t_path}.operation_filters: unknown operation '{op}' (allowed: {list(ALL_OPERATIONS)})")
        if "operations" in table_cfg and op not in operations:
            fail(f"{t_path}.operation_filters: '{op}' has a filter override but is not in 'operations' {operations}")
        check_filters(filters, known_filters, f"{t_path}.operation_filters.{op}")

    columns = table_cfg.get("columns", {})
    if not isinstance(columns, dict):
        fail(f"{t_path}.columns: expected an object mapping column name to config")
    for column, column_cfg in columns.items():
        c_path = f"{t_path}.columns.{column}"
        if not isinstance(column_cfg, dict):
            fail(f"{c_path}: expected an object")
        for key in column_cfg:
            if key not in COLUMN_KEYS:
                fail(f"{c_path}: unknown key '{key}' (allowed: {sorted(COLUMN_KEYS)})")
        if "filter_ops" in column_cfg:
            ops = column_cfg["filter_ops"]
            if not isinstance(ops, list):
                fail(f"{c_path}.filter_ops: expected a list, got {type(ops).__name__}")
            for i, op in enumerate(ops):
                if op not in ALL_FILTER_OPS:
                    fail(f"{c_path}.filter_ops[{i}]: unknown filter op '{op}' (allowed: {list(ALL_FILTER_OPS)})")
        for flag in ("insertable", "updatable"):
            if flag in column_cfg and not isinstance(column_cfg[flag], bool):
                fail(f"{c_path}.{flag}: expected a boolean, got {type(column_cfg[flag]).__name__}")

    for key in ("tenant_column", "read_relation", "write_table", "default_order"):
        if key in table_cfg and (not isinstance(table_cfg[key], str) or not table_cfg[key]):
            fail(f"{t_path}.{key}: expected a non-empty string")
    if "max_limit" in table_cfg and (type(table_cfg["max_limit"]) is not int or table_cfg["max_limit"] <= 0):
        fail(f"{t_path}.max_limit: expected a positive integer")


# --- live-DB structural introspection (facts, never intent) -------------

def fetch_table_exists(conn, schema, table):
    with conn.cursor() as cur:
        cur.execute(
            """
            SELECT 1 FROM information_schema.tables
            WHERE table_schema = %s AND table_name = %s
            """,
            (schema, table),
        )
        return cur.fetchone() is not None


def fetch_columns(conn, schema, table):
    with conn.cursor() as cur:
        cur.execute(
            """
            SELECT c.column_name, c.udt_name,
                   pgd.description
            FROM information_schema.columns c
            LEFT JOIN pg_catalog.pg_statio_all_tables st
                ON st.schemaname = c.table_schema AND st.relname = c.table_name
            LEFT JOIN pg_catalog.pg_description pgd
                ON pgd.objoid = st.relid AND pgd.objsubid = c.ordinal_position
            WHERE c.table_schema = %s AND c.table_name = %s
            ORDER BY c.ordinal_position
            """,
            (schema, table),
        )
        return cur.fetchall()


def fetch_primary_key(conn, schema, table):
    with conn.cursor() as cur:
        cur.execute(
            """
            SELECT kcu.column_name
            FROM information_schema.table_constraints tc
            JOIN information_schema.key_column_usage kcu
              ON kcu.constraint_name = tc.constraint_name AND kcu.table_schema = tc.table_schema
            WHERE tc.table_schema = %s AND tc.table_name = %s AND tc.constraint_type = 'PRIMARY KEY'
            """,
            (schema, table),
        )
        row = cur.fetchone()
        return row[0] if row else None


def fetch_fk_to_organisations(conn, schema, table):
    """Columns with a foreign key referencing core.organisations (any column).

    The common case (`organisation` / `organisation_id` by name) is checked
    first; this covers renamed tenant columns that still point at the
    organisations table.
    """
    with conn.cursor() as cur:
        cur.execute(
            """
            SELECT kcu.column_name
            FROM information_schema.table_constraints tc
            JOIN information_schema.key_column_usage kcu
              ON kcu.constraint_name = tc.constraint_name
             AND kcu.table_schema = tc.table_schema
            JOIN information_schema.constraint_column_usage ccu
              ON ccu.constraint_name = tc.constraint_name
             AND ccu.table_schema = tc.table_schema
            WHERE tc.table_schema = %s AND tc.table_name = %s
              AND tc.constraint_type = 'FOREIGN KEY'
              AND ccu.table_schema = 'core' AND ccu.table_name = 'organisations'
            """,
            (schema, table),
        )
        row = cur.fetchone()
        return row[0] if row else None


def resolve_tenant_column(conn, manifest_path, schema, table, table_cfg, columns):
    if "tenant_column" in table_cfg:
        tenant_column = table_cfg["tenant_column"]
        if tenant_column not in {c[0] for c in columns}:
            fail(
                f"{manifest_path}: schemas.{schema}.tables.{table}: "
                f"tenant_column '{tenant_column}' does not exist on the live table"
            )
        return tenant_column
    tenant_column = next((c[0] for c in columns if c[0] in TENANT_COLUMN_CANDIDATES), None)
    if tenant_column is None:
        tenant_column = fetch_fk_to_organisations(conn, schema, table)
    if tenant_column is None:
        fail(
            f"{manifest_path}: schemas.{schema}.tables.{table}: no tenant column detected "
            f"(looked for {sorted(TENANT_COLUMN_CANDIDATES)} and an FK to core.organisations) — "
            f"set an explicit 'tenant_column' in the manifest, or remove the table and write a manual handler"
        )
    return tenant_column


def build_view_spec(conn, manifest_path, schema, table, table_cfg, default_filters):
    t_path = f"{manifest_path}: schemas.{schema}.tables.{table}"
    if not fetch_table_exists(conn, schema, table):
        fail(f"{t_path}: table '{schema}.{table}' does not exist in the live database")

    operations = table_cfg.get("operations", list(ALL_OPERATIONS))
    if not operations:
        print(f"skip (no operations enabled in manifest): {schema}.{table}")
        return None

    columns = fetch_columns(conn, schema, table)
    pk = fetch_primary_key(conn, schema, table)
    if pk is None:
        fail(f"{t_path}: table '{schema}.{table}' has no primary key — it cannot back a CRUD view")

    col_by_name = {c[0]: c for c in columns}
    tenant_column = resolve_tenant_column(conn, manifest_path, schema, table, table_cfg, columns)

    for column in table_cfg.get("columns", {}):
        if column not in col_by_name:
            fail(f"{t_path}.columns.{column}: column does not exist on the live table '{schema}.{table}'")
        if column in SENSITIVE_COLUMNS:
            fail(
                f"{t_path}.columns.{column}: '{column}' is a SENSITIVE_COLUMNS member and is hard-omitted "
                f"from the API surface — it cannot be listed in the manifest"
            )

    pk_type = PG_TYPE_MAP.get(col_by_name[pk][1], "Text")
    if col_by_name[pk][1] not in PG_TYPE_MAP:
        print(f"warn: unmapped pk type '{col_by_name[pk][1]}' on {schema}.{table}.{pk}, using Text")

    manifest_columns = table_cfg.get("columns", {})
    fields = []
    for name, udt, _comment in columns:
        if name in (pk, tenant_column):
            continue
        if name in SENSITIVE_COLUMNS:
            continue
        col_cfg = manifest_columns.get(name, {})
        pg_type = PG_TYPE_MAP.get(udt, "Text")
        if udt not in PG_TYPE_MAP:
            print(f"warn: unmapped type '{udt}' on {schema}.{table}.{name}, using Text")
        fields.append(
            {
                "name": name,
                "type": pg_type,
                "filter_ops": col_cfg.get("filter_ops", []),
                "insertable": col_cfg.get("insertable", True),
                "updatable": col_cfg.get("updatable", True),
            }
        )

    op_filters_cfg = table_cfg.get("operation_filters", {})
    enabled = [op for op in ALL_OPERATIONS if op in operations]
    route = f"/api/v1/{table.replace('_', '-')}"
    class_name = "".join(p.capitalize() for p in table.split("_")) + "View"
    return {
        "name": class_name,
        "table": table,
        "route": route,
        "read_relation": table_cfg.get("read_relation", f"{schema}.{table}"),
        "write_table": table_cfg.get("write_table", f"{schema}.{table}"),
        "tenant_column": tenant_column,
        "pk": pk,
        "pk_type": pk_type,
        "default_order": table_cfg.get("default_order", pk),
        "max_limit": table_cfg.get("max_limit", 500),
        "fields": fields,
        "collection_routes": [
            {
                "handler": OP_HANDLERS[op],
                "method": OP_METHODS[op],
                "filters": op_filters_cfg.get(op, default_filters[op]),
            }
            for op in COLLECTION_OPS
            if op in enabled
        ],
        "item_routes": [
            {
                "handler": OP_HANDLERS[op],
                "method": OP_METHODS[op],
                "filters": op_filters_cfg.get(op, default_filters[op]),
            }
            for op in ITEM_OPS
            if op in enabled
        ],
    }


FIELD_LINE = '        {{"{name}", FieldType::{type}, {ops}, {insertable}, {updatable}}},'


def op_expr(ops):
    if not ops:
        return "0"
    if len(ops) == 1:
        # A bare `Op::X` does not implicitly convert to the uint8_t
        # filter_ops field; the cast keeps single-operator columns compiling.
        return f"static_cast<uint8_t>(Op::{ops[0].capitalize()})"
    return " | ".join(f"Op::{op.capitalize()}" for op in ops)


def filter_expr(filters):
    return "{" + ", ".join(f'"{f}"' for f in filters) + "}"


def route_line(route, name, entry, item=False):
    path = f"{route}/{{id}}" if item else route
    return (
        f'        {{"{path}", &{name}::{entry["handler"]}, '
        f'{{{entry["method"]}}}, {filter_expr(entry["filters"])}}},'
    )


HEADER_TEMPLATE = """// GENERATED by generate_views.py — DO NOT EDIT.
// Source: {schema}.{table}
#pragma once
#include <sgrn/datastore/handlers/CrudViewHandler.hpp>

#include <array>

namespace sgrn::datastore::handlers::query
{{
using sgrn::datastore::query::CrudViewSpec;
using sgrn::datastore::query::Field;
using sgrn::datastore::query::FieldType;
using sgrn::datastore::query::Op;
using sgrn::datastore::query::PrimaryKey;

class {name} : public CrudViewHandler<{name}> {{
public:
    static constexpr std::array<Field, {field_count}> kFields = {{{{
{fields}
    }}}};

    static constexpr CrudViewSpec kSpec{{
        .read_relation = "{read_relation}",
        .write_table = "{write_table}",
        .tenant_column = "{tenant_column}",
        .pk = {{"{pk}", FieldType::{pk_type}}},
        .fields = kFields,
        .default_order = "{default_order}",
        .max_limit = {max_limit},
    }};

    {name}()
        : CrudViewHandler(this, kRoutes, kItemRoutes) {{
    }}

private:
    static inline const std::array<RouteConfig, {num_collection_routes}> kRoutes = {{{{
{collection_routes}
    }}}};
    static inline const std::array<ItemRouteConfig, {num_item_routes}> kItemRoutes = {{{{
{item_routes}
    }}}};
}};

}} // namespace sgrn::datastore::handlers::query
"""


def emit_header(spec, out_dir):
    fields_code = "\n".join(
        FIELD_LINE.format(
            name=f["name"],
            type=f["type"],
            ops=op_expr(f["filter_ops"]),
            insertable="true" if f["insertable"] else "false",
            updatable="true" if f["updatable"] else "false",
        )
        for f in spec["fields"]
    )
    code = HEADER_TEMPLATE.format(
        schema=spec["read_relation"].split(".")[0],
        table=spec["table"],
        name=spec["name"],
        field_count=len(spec["fields"]),
        fields=fields_code,
        read_relation=spec["read_relation"],
        write_table=spec["write_table"],
        tenant_column=spec["tenant_column"],
        pk=spec["pk"],
        pk_type=spec["pk_type"],
        default_order=spec["default_order"],
        max_limit=spec["max_limit"],
        num_collection_routes=len(spec["collection_routes"]),
        collection_routes="\n".join(route_line(spec["route"], spec["name"], e) for e in spec["collection_routes"]),
        num_item_routes=len(spec["item_routes"]),
        item_routes="\n".join(route_line(spec["route"], spec["name"], e, item=True) for e in spec["item_routes"]),
    )
    os.makedirs(out_dir, exist_ok=True)
    with open(f"{out_dir}/{spec['name']}.gen.hpp", "w") as f:
        f.write(code)


def update_registration_file(specs, reg_file):
    """Appends new `static <View> s_<table>;` lines inside
    initGeneratedViews(). Never removes a line — a line deleted by a human
    stays deleted across every future run."""
    header = (
        "// Hand-curated after first generation. This file is only ever\n"
        "// APPENDED to — a line you delete here stays deleted, even after\n"
        "// re-running the generator. If a table is later dropped from the\n"
        "// database, its line here will fail to compile — delete it then.\n"
        "//\n"
        "// Each view self-registers its routes in its constructor (the same\n"
        "// IHandler mechanism every other handler uses). The function-local\n"
        "// statics below mirror initHandlers(): they are constructed exactly once,\n"
        "// on the first initGeneratedViews() call from initHandlers(), i.e. after\n"
        "// config load and before drogon::app().run().\n"
        '#include "AllViews.gen.hpp"\n'
        "\n"
        "namespace sgrn::datastore::handlers::query\n"
        "{\n"
        "\n"
        "void initGeneratedViews() {\n"
    )
    footer = "}\n\n} // namespace sgrn::datastore::handlers::query\n"
    try:
        with open(reg_file) as f:
            existing = f.read()
    except FileNotFoundError:
        existing = header + footer

    new_lines = [f"    static {s['name']} s_{s['table']};\n" for s in specs if f"s_{s['table']};" not in existing]
    if not new_lines:
        return
    # Insert before the closing brace of initGeneratedViews(): the footer
    # itself starts with that brace, so inserting at the footer's start
    # places new lines inside the function body.
    footer_start = existing.rindex(footer)
    existing = existing[:footer_start] + "".join(new_lines) + existing[footer_start:]
    os.makedirs(os.path.dirname(reg_file) or ".", exist_ok=True)
    with open(reg_file, "w") as f:
        f.write(existing)
    for line in new_lines:
        print(f"new table registered: {line.strip()}")


def main():
    parser = argparse.ArgumentParser(description="Generate compile-time CRUD view handlers from crud/manifest.json + live schema")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5432)
    parser.add_argument("--dbname", default="sgrn")
    parser.add_argument("--user", default="sgrn_datastore")
    parser.add_argument(
        "--manifest",
        default=DEFAULT_MANIFEST,
        help="Path to crud/manifest.json (the source of truth for exposed tables, operations, filters, and column flags)",
    )
    parser.add_argument(
        "--schemas",
        nargs="*",
        default=None,
        help="Restrict generation to these manifest schemas (default: all schemas listed in the manifest)",
    )
    parser.add_argument("--out", default=GEN_DIR)
    parser.add_argument("--registration", default=None)
    parser.add_argument(
        "--password",
        default=os.environ.get("PGPASSWORD", DEFAULT_PASSWORD),
        help="Postgres password (or set PGPASSWORD in the environment)",
    )
    args = parser.parse_args()

    known_filters = scan_known_filters()
    manifest, default_filters = load_manifest(args.manifest, known_filters)

    if args.schemas is not None:
        for schema in args.schemas:
            if schema not in manifest["schemas"]:
                fail(f"manifest {args.manifest}: --schemas requested '{schema}', which is not listed under 'schemas'")

    try:
        import psycopg2
    except ImportError:
        print("generate_views.py requires psycopg2 (pip install psycopg2-binary)", file=sys.stderr)
        raise

    reg_file = args.registration or f"{args.out}/RegisteredViews.cpp"

    conn = psycopg2.connect(host=args.host, port=args.port, dbname=args.dbname, user=args.user, password=args.password)
    specs = []
    for schema, schema_cfg in manifest["schemas"].items():
        if args.schemas is not None and schema not in args.schemas:
            continue
        for table, table_cfg in schema_cfg.get("tables", {}).items():
            spec = build_view_spec(conn, args.manifest, schema, table, table_cfg, default_filters)
            if spec:
                specs.append(spec)
                emit_header(spec, args.out)

    agg = f"{args.out}/AllViews.gen.hpp"
    os.makedirs(args.out, exist_ok=True)
    with open(agg, "w") as f:
        # Sorted: matches clang-format's SortIncludes so regen is diff-stable.
        names = sorted(f'{s["name"]}.gen.hpp' for s in specs)
        f.write("// GENERATED by generate_views.py — DO NOT EDIT.\n")
        f.write("#pragma once\n" + "".join(f'#include "{n}"\n' for n in names))

    update_registration_file(specs, reg_file)


if __name__ == "__main__":
    main()
