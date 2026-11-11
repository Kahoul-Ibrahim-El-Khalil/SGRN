#!/usr/bin/env python3
"""Fail if crud/manifest.json lets non-admins write, or leaves sensitive columns writable.

Run from the repo root (or pass the manifest path as argv[1]). Exit 0 = policy
holds; exit 1 prints every violating "POLICY:" line.

NOTE: this script only inspects columns *named* in the manifest. The generator
(generate_views.py) enforces the complementary rule at regeneration time: for
any table with a write operation, every live column in SENSITIVE_COLUMNS must
be explicitly configured, so a newly added column can never silently inherit
the writable default."
"""
import json
import pathlib
import sys

ADMIN = {
    "sgrn::datastore::filters::OrgAdminFilter",
    "sgrn::datastore::filters::PlatformAdminFilter",
}
SENSITIVE = {
    "is_platform_admin",
    "role",
    "is_active",
    "status_users",
    "token",
    "deleted_at",
    "granted_by",
    "path",
    "storage_limit",
    "entry_count_limit",
    "rate_limit_rpm",
    "max_file_size_mb",
    "preferred_chunk_size_mb",
    "rate_limit_upload_rpm",
    "total_virtual_size",
    "total_real_size",
    "total_entry_count",
    "metadata",
}
WRITE_OPS = ("create", "update", "delete")


def main() -> int:
    manifest_path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "crud/manifest.json")
    try:
        manifest = json.loads(manifest_path.read_text())
    except FileNotFoundError:
        print(f"POLICY: manifest not found: {manifest_path}")
        return 1
    except json.JSONDecodeError as e:
        print(f"POLICY: {manifest_path}: invalid JSON: {e}")
        return 1

    defaults = manifest.get("default_filters", {})
    errors = []
    for schema, sdef in manifest.get("schemas", {}).items():
        for table, t in sdef.get("tables", {}).items():
            ops = t.get("operations", ["list", "get", "create", "update", "delete"])
            filters = t.get("operation_filters", {})
            for op in WRITE_OPS:
                if op in ops and not (set(filters.get(op, defaults.get(op, []))) & ADMIN):
                    errors.append(f"{schema}.{table}: '{op}' is not admin-only")
            if (
                "list" in ops
                and table in ("users", "automated_services")
                and not (set(filters.get("list", defaults.get("list", []))) & ADMIN)
            ):
                errors.append(f"{schema}.{table}: 'list' exposes personal/service data to non-admins")
            cols = t.get("columns", {})
            for name, c in cols.items():
                key = "status_users" if (table == "users" and name == "status") else name
                if key in SENSITIVE and (c.get("insertable", True) or c.get("updatable", True)):
                    errors.append(f"{schema}.{table}.{name}: sensitive column is writable")

    for e in errors:
        print("POLICY:", e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
