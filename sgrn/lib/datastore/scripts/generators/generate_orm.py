#!/usr/bin/env python3
import os
import sys
import time
import socket
import tempfile
import contextlib
import getpass
from pathlib import Path
import json
import argparse
import subprocess
import shutil

# sgrn/lib/datastore — derived from this script's location so the default
# target dir works regardless of the caller's CWD (invoke from the repo
# root, e.g. `python3 sgrn/lib/datastore/scripts/generators/...`).
DATASTORE_ROOT = Path(__file__).resolve().parent.parent.parent
REPO_ROOT = DATASTORE_ROOT.parent.parent.parent

# Defaults
DEFAULT_TARGET_DIR = str(DATASTORE_ROOT / "src" / "orm" / "models")
DEFAULT_SCHEMAS = ["core", "storage"]

# Database Defaults
DB_CONFIG_TEMPLATE = {
    "rdbms": "postgresql",
    "host": "127.0.0.1",
    "port": 5432,
    "dbname": "sgrn",
    "user": "sgrn_datastore",
    "passwd": "dracaeris",
    "tables": [],  # Empty list = Auto-discover all tables
    "schema": ""   # Will be replaced per loop
}

def findPgBinary(name: str) -> str | None:
    conda_prefix = os.environ.get("CONDA_PREFIX")
    if conda_prefix:
        for sub in ["bin", "Library/bin"]:
            p = Path(conda_prefix) / sub / name
            if p.exists():
                return str(p)
    which_bin = shutil.which(name)
    if which_bin:
        return which_bin
    home = Path.home()
    candidates = [
        home / "micromamba" / "envs" / "SGRN" / "bin" / name,
        home / ".micromamba" / "envs" / "SGRN" / "bin" / name,
    ]
    for v in [17, 16, 15, 14]:
        candidates.append(Path(f"/usr/lib/postgresql/{v}/bin/{name}"))
    for cand in candidates:
        if cand.exists():
            return str(cand)
    return None

def isPortOpen(host: str, port: int) -> bool:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            return s.connect_ex((host, port)) == 0
    except Exception:
        return False

def getFreePort(host: str = "127.0.0.1") -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((host, 0))
        return s.getsockname()[1]

@contextlib.contextmanager
def ephemeralPostgres():
    initdb_exe = findPgBinary("initdb")
    postgres_exe = findPgBinary("postgres")
    if not initdb_exe or not postgres_exe:
        raise RuntimeError("No PostgreSQL installation found (could not find 'initdb' and 'postgres'). Please ensure PostgreSQL is installed or activate the SGRN conda/micromamba environment.")

    with tempfile.TemporaryDirectory(prefix="sgrn-build-pg-") as tmp_dir:
        data_dir = Path(tmp_dir) / "data"
        log_file_path = Path(tmp_dir) / "pg.log"
        port = getFreePort("127.0.0.1")

        print(f">> Starting ephemeral PostgreSQL cluster on port {port}...")
        initdb_cmd = [
            initdb_exe,
            "-D", str(data_dir),
            "-U", "postgres",
            "--auth=trust",
            "--no-locale",
            "--encoding=UTF8",
        ]
        subprocess.run(initdb_cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        with open(log_file_path, "w") as log_f:
            proc = subprocess.Popen(
                [
                    postgres_exe,
                    "-D", str(data_dir),
                    "-p", str(port),
                    "-k", tmp_dir,
                    "-c", "listen_addresses=127.0.0.1",
                    "-c", "fsync=off",
                    "-c", "synchronous_commit=off",
                ],
                stdout=log_f,
                stderr=subprocess.STDOUT,
            )

        ready = False
        deadline = time.time() + 15
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            if isPortOpen("127.0.0.1", port):
                ready = True
                break
            time.sleep(0.2)

        if not ready:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            err_log = log_file_path.read_text() if log_file_path.exists() else ""
            raise RuntimeError(f"Ephemeral Postgres failed to start on port {port}. Log: {err_log}")

        try:
            yield "127.0.0.1", port, "postgres", ""
        finally:
            print(">> Stopping ephemeral PostgreSQL cluster...")
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

def detectSuperuser(host: str, port: int, requested_user: str | None, password: str = "") -> tuple[str, str]:
    candidates = []
    if requested_user:
        candidates.append(requested_user)
    env_super = os.environ.get("POSTGRES_SUPERUSER")
    if env_super and env_super not in candidates:
        candidates.append(env_super)
    try:
        current_os_user = getpass.getuser()
        if current_os_user not in candidates:
            candidates.append(current_os_user)
    except Exception:
        pass
    for fallback in ["sgrn_datastore", "postgres"]:
        if fallback not in candidates:
            candidates.append(fallback)

    env = os.environ.copy()
    if password:
        env["PGPASSWORD"] = password

    for user in candidates:
        for db in ["postgres", "template1", "sgrn"]:
            try:
                res = subprocess.run(
                    ["psql", "-h", host, "-p", str(port), "-U", user, "-d", db, "-c", "SELECT 1;"],
                    env=env,
                    capture_output=True,
                    text=True,
                )
                if res.returncode == 0:
                    return user, db
            except Exception:
                pass
    return requested_user or (candidates[0] if candidates else "postgres"), "postgres"

def dropDatabase(host, port, superuser, superuser_password, dbname):
    if not isPortOpen(host, port):
        return
    real_superuser, maint_db = detectSuperuser(host, port, superuser, superuser_password)
    env = os.environ.copy()
    if superuser_password:
        env["PGPASSWORD"] = superuser_password
    try:
        subprocess.run(
            ["psql", "-h", host, "-p", str(port), "-U", real_superuser, "-d", maint_db, "-c", f'DROP DATABASE IF EXISTS "{dbname}" WITH (FORCE);'],
            env=env,
            capture_output=True,
            text=True,
            check=True
        )
        print(f">> Dropped build database '{dbname}'")
    except Exception as e:
        print(f">> Warning: Failed to drop database '{dbname}': {e}")

def initBuildDatabase(host, port, dbname, user, password, superuser, superuser_password):
    init_db_script = REPO_ROOT / "init-db.py"
    if not init_db_script.exists():
        raise FileNotFoundError(f"init-db.py not found at {init_db_script}")
    
    real_superuser, _ = detectSuperuser(host, port, superuser, superuser_password)
    cmd = [
        sys.executable, str(init_db_script),
        "--host", host,
        "--port", str(port),
        "--dbname", dbname,
        "--user", user,
        "--password", password,
        "--superuser", real_superuser,
        "--superuser-password", superuser_password,
    ]
    print(f">> Initializing isolated build database '{dbname}' at {host}:{port} using role '{real_superuser}'...")
    subprocess.run(cmd, check=True)

def generateViews(host, port, dbname, user, password):
    views_script = DATASTORE_ROOT / "scripts" / "generators" / "generate_views.py"
    if not views_script.exists():
        print(f">> Warning: generate_views.py not found at {views_script}")
        return
    cmd = [
        sys.executable, str(views_script),
        "--host", host,
        "--port", str(port),
        "--dbname", dbname,
        "--user", user,
        "--password", password,
    ]
    print(f">> Generating CRUD views against '{dbname}'...")
    subprocess.run(cmd, check=True)

def generateModelOrm(schema, target_dir, db_config):
    print(f">> Generating standard tables for schema: {schema}")
    
    orm_schema_dir = os.path.join(target_dir, schema)
    
    # 1. Create directory (mkdir -p)
    os.makedirs(orm_schema_dir, exist_ok=True)
    
    # 2. Prepare config for this specific schema
    current_config = db_config.copy()
    current_config["schema"] = schema
    
    config_path = os.path.join(orm_schema_dir, "model.json")
    
    # 3. Write JSON config
    with open(config_path, 'w') as f:
        json.dump(current_config, f, indent=4)
    
    # 4. Run drogon_ctl
    try:
        cmd = ["drogon_ctl", "create", "model", orm_schema_dir]
        subprocess.run(cmd, check=True, input=b"y\n")
    except subprocess.CalledProcessError as e:
        print(f"Error generating model for {schema}: {e}")

def normalizeOrmNamespace(target_dir: str, actual_dbname: str, target_namespace: str = "sgrn"):
    if actual_dbname == target_namespace:
        return
    for root, _, files in os.walk(target_dir):
        for file in files:
            if file.endswith((".h", ".cc", ".json")):
                file_path = os.path.join(root, file)
                with open(file_path, "r", encoding="utf-8") as f:
                    content = f.read()

                content = content.replace(f"using namespace drogon_model::{actual_dbname}::", f"using namespace drogon_model::{target_namespace}::")
                content = content.replace(f"namespace {actual_dbname}", f"namespace {target_namespace}")
                content = content.replace(f"}} // namespace {actual_dbname}", f"}} // namespace {target_namespace}")
                content = content.replace(f"drogon_model::{actual_dbname}::", f"drogon_model::{target_namespace}::")
                if file.endswith(".json"):
                    content = content.replace(f'"{actual_dbname}"', f'"{target_namespace}"')

                with open(file_path, "w", encoding="utf-8") as f:
                    f.write(content)

def runGeneration(target_dbname, target_dir, db_config, clean, views):
    # Optional: Clean old models
    if clean and os.path.exists(target_dir):
        print(f"Cleaning {target_dir}...")
        shutil.rmtree(target_dir)

    # Ensure base directory exists
    os.makedirs(target_dir, exist_ok=True)

    # Generate for all schemas
    for schema in DEFAULT_SCHEMAS:
        generateModelOrm(schema, target_dir, db_config)

    # Normalize namespace back to project canonical 'sgrn'
    normalizeOrmNamespace(target_dir, target_dbname, "sgrn")

    if views:
        generateViews(
            host=db_config["host"],
            port=db_config["port"],
            dbname=target_dbname,
            user=db_config["user"],
            password=db_config["passwd"]
        )

    print("Model Generation done")

def main():
    # Parse command line flags
    parser = argparse.ArgumentParser(description="Generate Drogon ORM Models")
    parser.add_argument("--clean", action="store_true", help="Clean the target directory before generating")
    parser.add_argument("--build-db", nargs="?", const="sgrn_build_orm", default=None,
                        help="Create an isolated build database from scratch, run generation against it, and drop it on completion (default name: sgrn_build_orm)")
    parser.add_argument("--keep-db", action="store_true", help="Keep the build database instead of dropping it on completion")
    parser.add_argument("--views", action="store_true", help="Also generate compile-time CRUD views (generate_views.py) against the build database")
    parser.add_argument("--dbname", type=str, default=None, help="Database name (defaults to build-db name if --build-db is set, otherwise 'sgrn')")
    parser.add_argument("--user", type=str, default=DB_CONFIG_TEMPLATE["user"], help="Database user")
    parser.add_argument("--password", type=str, default=os.environ.get("POSTGRES_PASSWORD", DB_CONFIG_TEMPLATE["passwd"]), help="Database password")
    parser.add_argument("--superuser", type=str, default=os.environ.get("POSTGRES_SUPERUSER", "postgres"), help="PostgreSQL superuser")
    parser.add_argument("--superuser-password", type=str, default=os.environ.get("POSTGRES_SUPERUSER_PASSWORD", "dracaeris"), help="PostgreSQL superuser password")
    parser.add_argument("--host", type=str, default=DB_CONFIG_TEMPLATE["host"], help="Database host")
    parser.add_argument("--port", type=int, default=DB_CONFIG_TEMPLATE["port"], help="Database port")
    args = parser.parse_args()

    target_dir = DEFAULT_TARGET_DIR
    build_db_name = args.build_db
    target_dbname = args.dbname if args.dbname else (build_db_name if build_db_name else DB_CONFIG_TEMPLATE["dbname"])

    # If build-db is requested and host is loopback but port is not open, automatically spin up ephemeral postgres
    use_ephemeral = build_db_name and args.host in ("127.0.0.1", "localhost") and not isPortOpen(args.host, args.port)

    if use_ephemeral:
        with ephemeralPostgres() as (e_host, e_port, e_super, e_super_pass):
            db_config = DB_CONFIG_TEMPLATE.copy()
            db_config["dbname"] = target_dbname
            db_config["user"] = args.user
            db_config["passwd"] = args.password
            db_config["host"] = e_host
            db_config["port"] = e_port

            initBuildDatabase(
                host=e_host,
                port=e_port,
                dbname=target_dbname,
                user=args.user,
                password=args.password,
                superuser=e_super,
                superuser_password=e_super_pass
            )

            runGeneration(target_dbname, target_dir, db_config, args.clean, args.views)
    else:
        db_config = DB_CONFIG_TEMPLATE.copy()
        db_config["dbname"] = target_dbname
        db_config["user"] = args.user
        db_config["passwd"] = args.password
        db_config["host"] = args.host
        db_config["port"] = args.port

        try:
            if build_db_name:
                initBuildDatabase(
                    host=args.host,
                    port=args.port,
                    dbname=target_dbname,
                    user=args.user,
                    password=args.password,
                    superuser=args.superuser,
                    superuser_password=args.superuser_password
                )

            runGeneration(target_dbname, target_dir, db_config, args.clean, args.views)
        finally:
            if build_db_name and not args.keep_db:
                dropDatabase(
                    host=args.host,
                    port=args.port,
                    superuser=args.superuser,
                    superuser_password=args.superuser_password,
                    dbname=target_dbname
                )

if __name__ == "__main__":
    main()
