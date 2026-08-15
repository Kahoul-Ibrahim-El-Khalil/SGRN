import os
import shutil
import tempfile
import subprocess
import sys
from pathlib import Path
from scripts.common import (
    run, fwd, getCurrentUser, getCurrentHome
)
from scripts.config import (
    TEMPLATE_USER, TEMPLATE_HOME, SGRN_CONFIG_FILES, POSTGRES_CONFIG_FILES,
    MINIO_SERVICE_NAME, POSTGREST_SERVICE_NAME, MINIO_DEFAULT_USER, MINIO_DEFAULT_PASS,
    POSTGREST_VERSION, POSTGREST_URL_TEMPLATE, MINIO_GIT_URL,
    SGRN_USER_HOME, CONFIG_DIR, PG_DATA_DIR, POSTGRES_CONFIG_DIR, NGINX_CONFIG_DIR,
    SYSTEMD_CONFIG_DIR, EXTERN_DIR_NAME, BUILD_DIR_NAME, DESERTATION_FOLDERS,
    BACKEND_DIR
)
from scripts.db import loadEnv

def syncConfigs(root: Path, conda_prefix: str):
    print("\n[Sync] Syncing configurations...")
    user, home = getCurrentUser(), getCurrentHome()
    sgrn_home = SGRN_USER_HOME
    sgrn_home.mkdir(parents=True, exist_ok=True)

    def dynamicCopy(src: Path, dst: Path):
        content = src.read_text().replace(TEMPLATE_HOME, home).replace(TEMPLATE_USER, user)
        dst.write_text(content)

    for f in SGRN_CONFIG_FILES:
        src = CONFIG_DIR / f
        if src.exists(): dynamicCopy(src, sgrn_home / f)
    
    pg_data = PG_DATA_DIR
    if pg_data.exists():
        pg_src = POSTGRES_CONFIG_DIR
        for f in POSTGRES_CONFIG_FILES:
            src = pg_src / f
            if src.exists(): dynamicCopy(src, pg_data / f)

    if conda_prefix:
        nginx_dst = Path(conda_prefix) / "etc" / "nginx"
        nginx_dst.mkdir(parents=True, exist_ok=True)
        src_nginx = NGINX_CONFIG_DIR
        if src_nginx.exists():
            for item in src_nginx.rglob("*"):
                if item.is_file():
                    target = nginx_dst / item.relative_to(src_nginx)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    dynamicCopy(item, target)

def deploySystemd(root: Path):
    print("[Systemd] Deploying Systemd services...")
    src_dir = SYSTEMD_CONFIG_DIR
    dst_dir = Path("/etc/systemd/system")
    user, home = getCurrentUser(), getCurrentHome()
    if not src_dir.exists(): return

    env_vars = loadEnv(root)
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_path = Path(tmp_dir)
        for svc in src_dir.glob("*.service"):
            content = svc.read_text().replace(f"User={TEMPLATE_USER}", f"User={user}").replace(f"Group={TEMPLATE_USER}", f"Group={user}").replace(TEMPLATE_HOME, home)
            if svc.name == MINIO_SERVICE_NAME:
                content = content.replace(f'Environment="MINIO_ROOT_USER={MINIO_DEFAULT_USER}"', f'Environment="MINIO_ROOT_USER={env_vars.get("MINIO_ROOT_USER", MINIO_DEFAULT_USER)}"')
                content = content.replace(f'Environment="MINIO_ROOT_PASSWORD={MINIO_DEFAULT_PASS}"', f'Environment="MINIO_ROOT_PASSWORD={env_vars.get("MINIO_ROOT_PASSWORD", MINIO_DEFAULT_PASS)}"')
            processed_svc = tmp_path / svc.name; processed_svc.write_text(content)
            run(["sudo", "cp", str(processed_svc), str(dst_dir / svc.name)], label=f"deploy {svc.name}")
    run(["sudo", "systemctl", "daemon-reload"], label="daemon-reload")

def buildMinio(root: Path, conda_prefix: str) -> bool:
    print("[MinIO] Building MinIO...")
    minio_dir = root / EXTERN_DIR_NAME / "minio"
    target = Path(conda_prefix) / "bin" / "minio"
    if target.exists(): return True
    if not minio_dir.exists():
        if not run(["git", "clone", "--depth", "1", MINIO_GIT_URL, str(minio_dir)], label="clone minio"):
            return False
    env = os.environ.copy(); env["GOBIN"] = str(Path(conda_prefix) / "bin"); env["CGO_ENABLED"] = "0"
    return run(["go", "install", "."], cwd=str(minio_dir), env=env, label="go install minio")

def installPostgrest(root: Path, conda_prefix: str) -> bool:
    print("[PostgREST] Installing PostgREST...")
    target = Path(conda_prefix) / "bin" / "postgrest"
    if target.exists(): return True
    url = POSTGREST_URL_TEMPLATE.format(v=POSTGREST_VERSION)
    tmp_tar = root / BUILD_DIR_NAME / "postgrest.tar.xz"; tmp_tar.parent.mkdir(parents=True, exist_ok=True)
    if run(["curl", "-L", "-o", str(tmp_tar), url], label="download postgrest"):
        if run(["tar", "-xJf", str(tmp_tar), "-C", str(Path(conda_prefix) / "bin")], label="extract postgrest"):
            tmp_tar.unlink(); return target.exists()
    return False

def syncDesertations(root: Path, targets: list[str] = None):
    print("[Desers] Building Desertations...")
    ds = root / "desertation" / "build.py"
    cmd = [sys.executable, str(ds)]
    if targets:
        cmd.extend(targets)
    if ds.exists(): subprocess.run(cmd, check=True)
    dst = root / BUILD_DIR_NAME / "desertations"
    dst.mkdir(parents=True, exist_ok=True)
    for p in DESERTATION_FOLDERS:
        pdf = root / "desertation" / p / "build" / "main.pdf"
        if pdf.exists(): shutil.copy2(pdf, dst / f"{p.replace(' ', '_')}.pdf")
