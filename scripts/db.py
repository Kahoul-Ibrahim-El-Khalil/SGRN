import os
import socket
import subprocess
import sys
import time
import contextlib
import shutil
import tempfile
from pathlib import Path
from scripts.common import (
    run, rmtree
)
from scripts.config import (
    POSTGRES_PORT, POSTGRES_HOST, POSTGRES_STARTUP_TIMEOUT,
    DEFAULT_DB_PASSWORD, TIMESCALEDB_GIT_URL, PARALLEL_JOBS,
    PG_DATA_DIR, POSTGRES_DIR, ORM_GEN_SCRIPT, EXTERN_DIR_NAME,
    BACKEND_DIR
)

def loadEnv(root: Path) -> dict[str, str]:
    env_vars = {}
    env_file = root / ".env"
    if env_file.exists():
        for line in env_file.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, val = line.split("=", 1)
                env_vars[key.strip()] = val.strip()
    return env_vars

def get_exe(name: str, conda_prefix: str) -> str:
    if not conda_prefix:
        return name
    prefix = Path(conda_prefix)
    for subdir in ["bin", "Library/bin"]:
        exe = prefix / subdir / name
        if exe.exists():
            return str(exe)
    return name

def get_free_port(host: str = "127.0.0.1") -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((host, 0))
        return s.getsockname()[1]

def wait_for_postgres(host: str, port: int, timeout_s: int = POSTGRES_STARTUP_TIMEOUT) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(1)
                s.connect((host, port))
                return True
        except Exception:
            time.sleep(0.5)
    return False

@contextlib.contextmanager
def temporaryPostgres(pg_data: Path, conda_prefix: str = "", port: int | None = None):
    print("[Postgres] Starting temporary Postgres...")
    log_path = pg_data / "startup.log"
    postgres_exe = get_exe("postgres", conda_prefix)
    postgres_port = port or get_free_port(POSTGRES_HOST)
    with open(log_path, "w") as log_file:
        proc = subprocess.Popen(
            [postgres_exe, "-D", str(pg_data), "-p", str(postgres_port),
             "-c", f"listen_addresses={POSTGRES_HOST}", "-c", "fsync=off",
             "-c", "shared_preload_libraries=timescaledb"],
            stdout=log_file, stderr=subprocess.STDOUT, text=True,
        )
        ready = False
        for _ in range(POSTGRES_STARTUP_TIMEOUT):
            if proc.poll() is not None: break
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                    s.settimeout(1); s.connect((POSTGRES_HOST, postgres_port))
                    ready = True; break
            except Exception:
                sys.stdout.write("."); sys.stdout.flush()
                time.sleep(0.5)
        if not ready:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
            raise RuntimeError("Postgres timeout")
        try: yield proc, postgres_port
        finally:
            print("[Postgres] Stopping temporary Postgres...")
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

def dbInit(root: Path, conda_prefix: str) -> bool:
    print("[DB] Initializing PostgreSQL Database...")
    pg_data = PG_DATA_DIR
    if not pg_data.exists():
        pg_data.parent.mkdir(parents=True, exist_ok=True)
        initdb_exe = get_exe("initdb", conda_prefix)
        if not run([initdb_exe, "-D", str(pg_data)], label="initdb"):
            return False

    conf_file = pg_data / "postgresql.conf"
    if conf_file.exists():
        content = conf_file.read_text()
        if "shared_preload_libraries" not in content or "timescaledb" not in content:
            with open(conf_file, "a") as f:
                f.write("\n# Added by SGRN db-init\nshared_preload_libraries = 'timescaledb'\n")

    db_pass = loadEnv(root).get("DATABASE_PASSWORD", DEFAULT_DB_PASSWORD)
    pg_ctl_exe = get_exe("pg_ctl", conda_prefix)
    status = subprocess.run([pg_ctl_exe, "-D", str(pg_data), "status"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if status.returncode == 0:
        restart_cmd = [pg_ctl_exe, "-D", str(pg_data), "-m", "fast", "restart", "-w"]
        if not run(restart_cmd, label="restart postgres cluster"):
            return False
    else:
        start_cmd = [pg_ctl_exe, "-D", str(pg_data), "-l", str(pg_data / "startup.log"), "-w", "start"]
        if not run(start_cmd, label="start postgres cluster"):
            return False

    if not wait_for_postgres(POSTGRES_HOST, POSTGRES_PORT):
        print("[ERROR] postgres did not become ready on the expected port after restart")
        return False

    sql_dir = POSTGRES_DIR
    env = os.environ.copy(); env["PGPASSWORD"] = db_pass
    psql_exe = get_exe("psql", conda_prefix)
    if not run([psql_exe, "-h", POSTGRES_HOST, "-p", str(POSTGRES_PORT), "-d", "postgres", "-f", "init.sql"],
               label="psql init.sql", cwd=str(sql_dir), env=env):
        return False
    return True

def buildOrm(root: Path, conda_prefix: str = ""):
    print("[ORM] Generating Drogon ORM Models...")
    pg_data = PG_DATA_DIR
    if not pg_data.exists(): return
    with tempfile.TemporaryDirectory(prefix="sgrn-orm-pgdata-") as tmp_dir:
        temp_pg_data = Path(tmp_dir) / "data"
        shutil.copytree(pg_data, temp_pg_data, ignore=shutil.ignore_patterns("postmaster.pid", "startup.log"))
        with temporaryPostgres(temp_pg_data, conda_prefix, port=get_free_port(POSTGRES_HOST)) as (_proc, postgres_port):
            env = os.environ.copy(); env["PGPASSWORD"] = loadEnv(root).get("DATABASE_PASSWORD", DEFAULT_DB_PASSWORD)
            psql_exe = get_exe("psql", conda_prefix)
            if not run([psql_exe, "-h", POSTGRES_HOST, "-p", str(postgres_port), "-d", "postgres", "-f", "init.sql"],
                       cwd=str(POSTGRES_DIR), env=env, label="psql init.sql (orm)"):
                return
            gen_script = ORM_GEN_SCRIPT
            if gen_script.exists():
                run([sys.executable, str(gen_script), "--clean", "--host", POSTGRES_HOST, "--port", str(postgres_port)],
                    cwd=str(BACKEND_DIR),
                    label="generate_orm")

def buildTimescaleDB(root: Path, conda_prefix: str) -> bool:
    print("[TimescaleDB] Building TimescaleDB from source...")
    ts_dir = root / EXTERN_DIR_NAME / "timescaledb"
    if not ts_dir.exists():
        if not run(["git", "clone", "--depth", "1", TIMESCALEDB_GIT_URL, str(ts_dir)], label="clone timescaledb"):
            return False
    
    build_dir = ts_dir / "build"
    if build_dir.exists(): rmtree(build_dir)
    pg_config = get_exe("pg_config", conda_prefix)
    env = os.environ.copy(); env["CC"] = "gcc"
    
    if not run(["./bootstrap", "-DREGRESS_CHECKS=OFF", f"-DPG_CONFIG={pg_config}"], cwd=str(ts_dir), env=env, label="timescaledb bootstrap"): return False
    if not run(["make", "-j", str(PARALLEL_JOBS)], cwd=str(build_dir), label="timescaledb make"): return False
    return run(["make", "install"], cwd=str(build_dir), label="timescaledb install")
