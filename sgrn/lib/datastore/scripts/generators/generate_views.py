#!/usr/bin/env python3
# generate_views.py — compile-time CRUD view generator (manual step).
#
# Connects to a live (dev/migrated) Postgres instance (same connection
# pattern as generate_orm.py), introspects columns/PK/comments, emits one
# CrudViewSpec-based handler header per eligible table, and appends (never
# removes) one registration line per newly-seen table.
#
# NOT wired into the CMake build as a live-DB dependency — run manually
# after schema changes, exactly like generate_orm.py is today; output is
# committed like the ORM models are.
#
# Filter intent lives as COMMENT ON COLUMN next to the DDL that defines the
# column (see postgres/schemas/core.sql "sgrn: CRUD view annotations").
# The database is the source of truth; there is no separate manifest file.
import argparse
import os
import re
import sys

DEFAULT_SCHEMAS = ["core", "storage"]
TENANT_COLUMN_CANDIDATES = {"organisation", "organisation_id"}

# Columns that are never part of the API surface — not readable via the
# whitelist SELECT list, not filterable, not writable — even though they
# exist on the table. The engine's explicit column list (pk + tenant +
# fields) guarantees they are never returned. A `sgrn: readonly` comment on
# these columns documents the intent in the schema; the omission here
# enforces it.
SENSITIVE_COLUMNS = {"password", "token_secret_hash", "password_hash", "secret"}

GEN_DIR = "src/handlers/generated"
REGISTRATION_FILE = f"{GEN_DIR}/RegisteredViews.cpp"

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


def fetch_table_comment(conn, schema, table):
    with conn.cursor() as cur:
        cur.execute("SELECT obj_description(%s::regclass)", (f"{schema}.{table}",))
        row = cur.fetchone()
        return row[0] if row else None


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


def parse_tags(comment):
    """'sgrn: filter=eq,gte,lte' -> {'filter': ['eq','gte','lte']}
    'sgrn: readonly'          -> {'readonly': True}"""
    tags = {}
    if not comment:
        return tags
    m = re.search(r"sgrn:\s*(.+)", comment)
    if not m:
        return tags
    for part in m.group(1).split(";"):
        part = part.strip()
        if part == "readonly":
            tags["readonly"] = True
        elif part.startswith("filter="):
            tags["filter"] = [o.strip() for o in part.split("=", 1)[1].split(",") if o.strip()]
    return tags


def build_view_spec(conn, schema, table):
    table_tags = parse_tags(fetch_table_comment(conn, schema, table))
    if table_tags.get("readonly"):
        print(f"skip (table marked readonly): {schema}.{table}")
        return None

    columns = fetch_columns(conn, schema, table)
    pk = fetch_primary_key(conn, schema, table)
    if pk is None:
        print(f"skip (no primary key): {schema}.{table}")
        return None

    col_by_name = {c[0]: c for c in columns}
    tenant_column = next((c[0] for c in columns if c[0] in TENANT_COLUMN_CANDIDATES), None)
    if tenant_column is None:
        tenant_column = fetch_fk_to_organisations(conn, schema, table)
    if tenant_column is None:
        print(f"skip (no tenant column detected — write a manual handler): {schema}.{table}")
        return None

    pk_type = PG_TYPE_MAP.get(col_by_name[pk][1], "Text")
    if col_by_name[pk][1] not in PG_TYPE_MAP:
        print(f"warn: unmapped pk type '{col_by_name[pk][1]}' on {schema}.{table}.{pk}, using Text")

    fields = []
    for name, udt, comment in columns:
        if name in (pk, tenant_column):
            continue
        if name in SENSITIVE_COLUMNS:
            continue
        tags = parse_tags(comment)
        pg_type = PG_TYPE_MAP.get(udt, "Text")
        if udt not in PG_TYPE_MAP:
            print(f"warn: unmapped type '{udt}' on {schema}.{table}.{name}, using Text")
        fields.append(
            {
                "name": name,
                "type": pg_type,
                "filter_ops": tags.get("filter", []),
                "insertable": not tags.get("readonly", False),
                "updatable": not tags.get("readonly", False),
            }
        )

    class_name = "".join(p.capitalize() for p in table.split("_")) + "View"
    return {
        "name": class_name,
        "table": table,
        "route": f"/api/v1/{table.replace('_', '-')}",
        "read_relation": f"{schema}.{table}",
        "write_table": f"{schema}.{table}",
        "tenant_column": tenant_column,
        "pk": pk,
        "pk_type": pk_type,
        "fields": fields,
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
        .default_order = "{pk}",
        .max_limit = 500,
    }};

    {name}()
        : CrudViewHandler(this, kRoutes, kItemRoutes) {{
    }}

private:
    static inline const std::array<RouteConfig, 2> kRoutes = {{{{
        {{"{route}", &{name}::handleList, {{drogon::Get}}, {{"sgrn::datastore::filters::UserAuthFilter"}}}},
        {{"{route}", &{name}::handleCreate, {{drogon::Post}}, {{"sgrn::datastore::filters::UserAuthFilter"}}}},
    }}}};
    static inline const std::array<ItemRouteConfig, 3> kItemRoutes = {{{{
        {{"{route}/{{id}}", &{name}::handleGet, {{drogon::Get}}, {{"sgrn::datastore::filters::UserAuthFilter"}}}},
        {{"{route}/{{id}}", &{name}::handleUpdate, {{drogon::Patch}}, {{"sgrn::datastore::filters::UserAuthFilter"}}}},
        {{"{route}/{{id}}", &{name}::handleDelete, {{drogon::Delete}}, {{"sgrn::datastore::filters::UserAuthFilter"}}}},
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
        route=spec["route"],
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
    parser = argparse.ArgumentParser(description="Generate compile-time CRUD view handlers from live schema")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5432)
    parser.add_argument("--dbname", default="sgrn")
    parser.add_argument("--user", default="sgrn_datastore")
    parser.add_argument("--schemas", nargs="*", default=DEFAULT_SCHEMAS)
    parser.add_argument("--out", default=GEN_DIR)
    parser.add_argument("--registration", default=None)
    parser.add_argument(
        "--password",
        default=os.environ.get("PGPASSWORD"),
        help="Postgres password (or set PGPASSWORD in the environment)",
    )
    args = parser.parse_args()
    if not args.password:
        parser.error("--password is required (or set PGPASSWORD in the environment)")

    try:
        import psycopg2
    except ImportError:
        print("generate_views.py requires psycopg2 (pip install psycopg2-binary)", file=sys.stderr)
        raise

    reg_file = args.registration or f"{args.out}/RegisteredViews.cpp"

    conn = psycopg2.connect(host=args.host, port=args.port, dbname=args.dbname, user=args.user, password=args.password)
    specs = []
    for schema in args.schemas:
        with conn.cursor() as cur:
            cur.execute("SELECT table_name FROM information_schema.tables WHERE table_schema = %s", (schema,))
            tables = [r[0] for r in cur.fetchall()]
        for table in tables:
            spec = build_view_spec(conn, schema, table)
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
