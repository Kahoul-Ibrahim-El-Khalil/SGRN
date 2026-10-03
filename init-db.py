#!/usr/bin/env python3
"""
SGRN Database Initializer Script (init-db.py)

Initializes or resets the PostgreSQL database and applies all schema, role, view,
function, and seed SQL files directly from Python without requiring compilation
or running `sgrn_datastore --init-db`.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Paths relative to repo root
REPO_ROOT = Path(__file__).resolve().parent
POSTGRES_DIR = REPO_ROOT / "sgrn" / "lib" / "datastore" / "postgres"

sys.path.insert(0, str(REPO_ROOT))
try:
    from scripts.generate_embedded_assets import flattenSql
    from scripts.common import loadEnv
except ImportError:
    flattenSql = None
    def loadEnv(root: Path) -> dict:
        return {}

def flatten_sql_pure(file_path: Path, variables: dict, visited: set = None) -> str:
    if visited is None:
        visited = set()
    file_path = file_path.resolve()
    if file_path in visited:
        return ""
    visited.add(file_path)

    if not file_path.exists():
        raise FileNotFoundError(f"SQL file not found: {file_path}")

    output = []
    lines = file_path.read_text(encoding="utf-8").splitlines()
    for line in lines:
        stripped = line.strip()
        if stripped.startswith(r"\i ") or stripped.startswith(r"\ir "):
            include_rel_path = stripped.split(" ", 1)[1].strip(" '\"")
            include_abs_path = file_path.parent / include_rel_path
            output.append(f"-- INLINED: {include_rel_path}")
            output.append(flatten_sql_pure(include_abs_path, variables, visited))
        else:
            s_lower = stripped.lower()
            if (s_lower.startswith("drop database") or 
                s_lower.startswith("create database") or 
                s_lower.startswith("lc_collate") or 
                s_lower.startswith("lc_ctype") or 
                s_lower.startswith("encoding")):
                continue
            output.append(line)

    text = "\n".join(output)
    for k, v in variables.items():
        text = text.replace(f"${{{k}}}", str(v))
    return text

def flatten_sql(file_path: Path, variables: dict) -> str:
    return flatten_sql_pure(file_path, variables)


def main():
    env_vars = loadEnv(REPO_ROOT)
    
    parser = argparse.ArgumentParser(description="SGRN Database Initializer")
    parser.add_argument("--host", default=env_vars.get("POSTGRES_HOST", os.environ.get("POSTGRES_HOST", "localhost")), help="PostgreSQL host")
    parser.add_argument("--port", type=int, default=int(env_vars.get("POSTGRES_PORT", os.environ.get("POSTGRES_PORT", "5432"))), help="PostgreSQL port")
    parser.add_argument("--dbname", "-d", default=env_vars.get("POSTGRES_DB", os.environ.get("POSTGRES_DB", "sgrn")), help="Target Database Name")
    parser.add_argument("--user", "-u", default=env_vars.get("POSTGRES_USER", os.environ.get("POSTGRES_USER", "sgrn_datastore")), help="App Database User")
    parser.add_argument("--password", "-p", default=env_vars.get("POSTGRES_PASSWORD", os.environ.get("POSTGRES_PASSWORD", "dracaeris")), help="App Database Password")
    parser.add_argument("--superuser", default=env_vars.get("POSTGRES_SUPERUSER", os.environ.get("POSTGRES_SUPERUSER", "postgres")), help="PostgreSQL Superuser")
    parser.add_argument("--superuser-password", default=env_vars.get("POSTGRES_SUPERUSER_PASSWORD", os.environ.get("POSTGRES_SUPERUSER_PASSWORD", "dracaeris")), help="PostgreSQL Superuser Password")
    parser.add_argument("--sql-dir", default=str(POSTGRES_DIR), help="Path to postgres SQL directory")
    
    args = parser.parse_args()
    
    sql_dir = Path(args.sql_dir)
    main_init = sql_dir / "init.sql"
    
    if not main_init.exists():
        print(f"[ERROR] Main init script not found at {main_init}")
        sys.exit(1)

    print(f"[init-db] Initializing database '{args.dbname}' at {args.host}:{args.port}...")

    # Template variables
    variables = {
        "POSTGRES_DB": args.dbname,
        "POSTGRES_USER": args.user,
        "POSTGRES_PASSWORD": args.password,
    }

    # 1. Flatten SQL files
    print("[init-db] Resolving SQL schemas, roles, views, functions, and seed data...")
    try:
        flattened_sql = flatten_sql(main_init, variables)
    except Exception as e:
        print(f"[ERROR] Failed to flatten SQL files: {e}")
        sys.exit(1)

    # 2. Setup env for PGPASSWORD
    env = os.environ.copy()
    env["PGPASSWORD"] = args.superuser_password

    # 3. Drop and Recreate DB via psql or psycopg2
    print(f"[init-db] Recreating database '{args.dbname}'...")
    
    recreate_sql = f"""
    DROP DATABASE IF EXISTS "{args.dbname}" WITH (FORCE);
    CREATE DATABASE "{args.dbname}" ENCODING 'UTF8';
    """

    psql_available = False
    try:
        subprocess.run(
            ["psql", "-h", args.host, "-p", str(args.port), "-U", args.superuser, "-d", "postgres", "-c", f'DROP DATABASE IF EXISTS "{args.dbname}" WITH (FORCE);'],
            env=env,
            capture_output=True,
            text=True,
            check=True
        )
        subprocess.run(
            ["psql", "-h", args.host, "-p", str(args.port), "-U", args.superuser, "-d", "postgres", "-c", f'CREATE DATABASE "{args.dbname}" ENCODING \'UTF8\';'],
            env=env,
            capture_output=True,
            text=True,
            check=True
        )
        psql_available = True
    except FileNotFoundError:
        pass
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] Failed to recreate database via psql:\n{e.stderr}")
        sys.exit(1)

    if not psql_available:
        try:
            import psycopg2
            from psycopg2.extensions import ISOLATION_LEVEL_AUTOCOMMIT
            conn = psycopg2.connect(
                host=args.host, port=args.port, user=args.superuser, password=args.superuser_password, dbname="postgres"
            )
            conn.set_isolation_level(ISOLATION_LEVEL_AUTOCOMMIT)
            with conn.cursor() as cur:
                cur.execute(f'DROP DATABASE IF EXISTS "{args.dbname}" WITH (FORCE);')
                cur.execute(f'CREATE DATABASE "{args.dbname}" ENCODING \'UTF8\';')
            conn.close()
        except ImportError:
            print("[ERROR] Neither 'psql' binary nor 'psycopg2' library was found.")
            sys.exit(1)
        except Exception as e:
            print(f"[ERROR] Failed to recreate database: {e}")
            sys.exit(1)

    # 4. Apply flattened schema to the new DB
    print(f"[init-db] Applying flattened schema to database '{args.dbname}'...")
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as tf:
        tf.write(flattened_sql)
        temp_sql_path = tf.name

    try:
        if psql_available:
            res = subprocess.run(
                ["psql", "-h", args.host, "-p", str(args.port), "-U", args.superuser, "-d", args.dbname, "-f", temp_sql_path, "-v", "ON_ERROR_STOP=1"],
                env=env,
                capture_output=True,
                text=True,
                check=True
            )
        else:
            import psycopg2
            conn = psycopg2.connect(
                host=args.host, port=args.port, user=args.superuser, password=args.superuser_password, dbname=args.dbname
            )
            conn.autocommit = True
            with conn.cursor() as cur:
                cur.execute(flattened_sql)
            conn.close()
        print("[init-db] Database initialized successfully!")
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] Failed to execute SQL schema on '{args.dbname}':\n{e.stderr}")
        sys.exit(1)
    except Exception as e:
        print(f"[ERROR] Failed to execute SQL schema via psycopg2:\n{e}")
        sys.exit(1)
    finally:
        if os.path.exists(temp_sql_path):
            os.unlink(temp_sql_path)

if __name__ == "__main__":
    main()
