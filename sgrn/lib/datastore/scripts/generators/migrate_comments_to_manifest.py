#!/usr/bin/env python3
# migrate_comments_to_manifest.py — ONE-OFF migration (runs once, then keep
# for archaeology, do not wire into any build).
#
# Reads the legacy `sgrn: filter=...` / `sgrn: readonly` COMMENT ON COLUMN /
# COMMENT ON TABLE annotations from the live dev DB — the same way the old
# generate_views.py did — and emits crud/manifest.json reproducing today's
# generated output byte-for-byte (same tables, same filter_ops, same
# readonly flags, same hardcoded UserAuthFilter everywhere).
#
# Procedure:
#   1. Run this script against a dev DB migrated from the CURRENT schema
#      (which still carries the sgrn: comments).
#   2. Re-run generate_views.py with the emitted manifest.
#   3. `git diff` the regenerated .gen.hpp files against the committed ones:
#      expect zero behavioral drift (modulo clang-format line wrapping).
#   4. Only then: strip the sgrn: comments from postgres/schemas/*.sql and
#      hand-edit crud/manifest.json for the new capabilities (per-op
#      filters, partial operation sets).
#
# Tables skipped by the old generator are handled as follows:
#   - table-level `sgrn: readonly` (e.g. core.sessions) -> emitted with
#     `"operations": []`, the manifest spelling of the old table-readonly.
#   - no primary key (views) or no tenant column (core.organisations,
#     storage.*) -> omitted entirely; the manifest is an allow-list and the
#     new generator hard-errors on unresolvable tenant columns, so these
#     must stay absent.
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from generate_views import (  # noqa: E402
    ALL_OPERATIONS,
    DATASTORE_ROOT,
    DEFAULT_PASSWORD,
    SENSITIVE_COLUMNS,
    TENANT_COLUMN_CANDIDATES,
    USER_AUTH_FILTER,
    fetch_columns,
    fetch_fk_to_organisations,
    fetch_primary_key,
)

DEFAULT_SCHEMAS = ["core", "storage"]
SCHEMA_VERSION = 1


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


def fetch_table_comment(conn, schema, table):
    with conn.cursor() as cur:
        cur.execute("SELECT obj_description(%s::regclass)", (f"{schema}.{table}",))
        row = cur.fetchone()
        return row[0] if row else None


def fetch_tables(conn, schema):
    with conn.cursor() as cur:
        cur.execute("SELECT table_name FROM information_schema.tables WHERE table_schema = %s", (schema,))
        return [r[0] for r in cur.fetchall()]


def migrate_table(conn, schema, table):
    """Returns (table_name, table_cfg) or None if the table stays absent
    from the manifest. Mirrors the old build_view_spec() skip order:
    table-readonly first, then PK, then tenant column."""
    table_tags = parse_tags(fetch_table_comment(conn, schema, table))
    pk = fetch_primary_key(conn, schema, table)

    if table_tags.get("readonly"):
        if pk is None:
            print(f"skip (table marked readonly, but no PK — left absent): {schema}.{table}")
            return None
        print(f"migrate as operations=[] (table marked readonly): {schema}.{table}")
        return table, {"operations": []}

    columns = fetch_columns(conn, schema, table)
    if pk is None:
        print(f"skip (no primary key): {schema}.{table}")
        return None

    tenant_column = next((c[0] for c in columns if c[0] in TENANT_COLUMN_CANDIDATES), None)
    if tenant_column is None:
        tenant_column = fetch_fk_to_organisations(conn, schema, table)
    if tenant_column is None:
        print(f"skip (no tenant column detected): {schema}.{table}")
        return None

    col_cfg = {}
    for name, _udt, comment in columns:
        if name in (pk, tenant_column):
            continue
        if name in SENSITIVE_COLUMNS:
            continue
        tags = parse_tags(comment)
        entry = {}
        if "filter" in tags:
            entry["filter_ops"] = tags["filter"]
        if tags.get("readonly"):
            entry["insertable"] = False
            entry["updatable"] = False
        if entry:
            col_cfg[name] = entry

    table_cfg = {
        "tenant_column": tenant_column,
        "read_relation": f"{schema}.{table}",
        "write_table": f"{schema}.{table}",
        "default_order": pk,
        "max_limit": 500,
        "operations": list(ALL_OPERATIONS),
    }
    if col_cfg:
        table_cfg["columns"] = col_cfg
    return table, table_cfg


def main():
    parser = argparse.ArgumentParser(description="One-off: migrate sgrn: DB comments to crud/manifest.json")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5432)
    parser.add_argument("--dbname", default="sgrn")
    parser.add_argument("--user", default="sgrn_datastore")
    parser.add_argument("--schemas", nargs="*", default=DEFAULT_SCHEMAS)
    parser.add_argument("--out", default=str(DATASTORE_ROOT / "crud" / "manifest.json"))
    parser.add_argument(
        "--password",
        default=os.environ.get("PGPASSWORD", DEFAULT_PASSWORD),
        help="Postgres password (or set PGPASSWORD in the environment)",
    )
    args = parser.parse_args()

    try:
        import psycopg2
    except ImportError:
        print("migrate_comments_to_manifest.py requires psycopg2 (pip install psycopg2-binary)", file=sys.stderr)
        raise

    conn = psycopg2.connect(host=args.host, port=args.port, dbname=args.dbname, user=args.user, password=args.password)

    manifest = {
        "$schema_version": SCHEMA_VERSION,
        "default_filters": {op: [USER_AUTH_FILTER] for op in ALL_OPERATIONS},
        "schemas": {},
    }
    for schema in args.schemas:
        tables = {}
        for table in sorted(fetch_tables(conn, schema)):
            migrated = migrate_table(conn, schema, table)
            if migrated:
                tables[migrated[0]] = migrated[1]
        if tables:
            manifest["schemas"][schema] = {"tables": tables}

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
