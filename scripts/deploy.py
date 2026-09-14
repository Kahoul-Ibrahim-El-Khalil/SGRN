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
    SGRN_USER_HOME, CONFIG_DIR, PG_DATA_DIR, POSTGRES_CONFIG_DIR, NGINX_CONFIG_DIR,
    SYSTEMD_CONFIG_DIR, BUILD_DIR_NAME, DESERTATION_FOLDERS,
    BACKEND_DIR
)

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

    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_path = Path(tmp_dir)
        for svc in src_dir.glob("*.service"):
            content = svc.read_text().replace(f"User={TEMPLATE_USER}", f"User={user}").replace(f"Group={TEMPLATE_USER}", f"Group={user}").replace(TEMPLATE_HOME, home)
            processed_svc = tmp_path / svc.name; processed_svc.write_text(content)
            run(["sudo", "cp", str(processed_svc), str(dst_dir / svc.name)], label=f"deploy {svc.name}")
    run(["sudo", "systemctl", "daemon-reload"], label="daemon-reload")

def ensureGarage(root: Path) -> bool:
    """Check the prebuilt `garage` binary is on PATH (https://garagehq.deuxfleurs.fr,
    or `cargo install`). Unlike MinIO there is nothing to compile here."""
    print("[Garage] Checking for garage binary...")
    if shutil.which("garage"):
        return True
    print("[Garage] ERROR: `garage` not found on PATH. Install the prebuilt")
    print("  binary (~/bin or /usr/local/bin) then provision once per machine:")
    print("  garage -c $SGRN_DATA_DIR/garage/garage.toml layout assign -z dc1 -c 50G <node-id>")
    print("  garage -c $SGRN_DATA_DIR/garage/garage.toml layout apply --version 1")
    print("  garage -c $SGRN_DATA_DIR/garage/garage.toml key import $GARAGE_ACCESS_KEY $GARAGE_SECRET_KEY -n sgrn-datastore --yes")
    print("  garage -c $SGRN_DATA_DIR/garage/garage.toml bucket create sgrn-uploads")
    print("  garage -c $SGRN_DATA_DIR/garage/garage.toml bucket allow --read --write sgrn-uploads --key sgrn-datastore")
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
