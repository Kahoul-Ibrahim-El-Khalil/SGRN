#!/usr/bin/env python3

import os
import sys
import shutil
import subprocess
import pwd
from pathlib import Path

# Target system directory for systemd services
SYSTEMD_DIR = Path("/etc/systemd/system")

def check_root_privileges():
    """Checks if the script is run with root privileges."""
    if os.geteuid() != 0:
        print("Error: This script must be run with root privileges (e.g., using sudo).")
        sys.exit(1)

def get_sudo_user_info():
    """Resolves the user info of the original user running sudo."""
    sudo_user = os.environ.get("SUDO_USER")
    if not sudo_user:
        # Fallback if not run with sudo (though we checked root privileges, so it should be run with sudo)
        sudo_user = pwd.getpwuid(os.getuid()).pw_name
    
    try:
        user_info = pwd.getpwnam(sudo_user)
        return sudo_user, Path(user_info.pw_dir), user_info.pw_uid, user_info.pw_gid
    except KeyError:
        print(f"Error: Could not retrieve user info for user '{sudo_user}'")
        sys.exit(1)

def load_env_file(env_path: Path) -> dict:
    """Loads environment variables from the specified .env file path."""
    env_vars = {}
    if not env_path.exists():
        print(f"Error: Environment file not found at {env_path}")
        sys.exit(1)
        
    with open(env_path, "r") as f:
        for line in f:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if "=" in line:
                k, v = line.split("=", 1)
                env_vars[k.strip()] = v.strip()
    return env_vars

def should_copy(src: Path, dst: Path) -> bool:
    """Check if src should be copied to dst based on metadata."""
    if not dst.exists():
        return True
    s_stat = src.stat()
    d_stat = dst.stat()
    return s_stat.st_size != d_stat.st_size or s_stat.st_mtime != d_stat.st_mtime

def copy_if_changed(src_file: Path, dst_file: Path, user_uid: int, user_gid: int, is_nginx: bool = False):
    """
    Copies src_file to dst_file if dst_file doesn't exist or its metadata differs.
    Returns True if file was copied, False otherwise.
    """
    if not src_file.is_file():
        print(f"Warning: Source file {src_file} not found or inaccessible. Skipping.")
        return False

    if should_copy(src_file, dst_file):
        print(f"Copying {src_file.name} to {dst_file}...")
        try:
            # Ensure destination directory exists
            dst_file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src_file, dst_file)
            
            # If it's an nginx config, fix ownership to the user
            if is_nginx:
                os.chown(dst_file, user_uid, user_gid)
            
            return True
        except Exception as e:
            print(f"Error copying {src_file.name}: {e}")
            return False
    else:
        print(f"No changes detected for {src_file.name}.")
        return False

def reload_systemd_services(changed_services):
    """Reloads the systemd daemon and restarts/reloads the specified services."""
    print("\nReloading systemd daemon...")
    try:
        subprocess.run(["systemctl", "daemon-reload"], check=True)
        print("Systemd daemon reloaded successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Error reloading systemd daemon: {e}")
        return

    for service_name in changed_services:
        print(f"Restarting service: {service_name}...")
        try:
            subprocess.run(["systemctl", "restart", service_name], check=True)
            print(f"Service {service_name} restarted successfully.")
        except subprocess.CalledProcessError as e:
            print(f"Error restarting {service_name}: {e}. Attempting to reload...")
            try:
                subprocess.run(["systemctl", "reload", service_name], check=True)
                print(f"Service {service_name} reloaded successfully.")
            except subprocess.CalledProcessError as e_reload:
                print(f"Error reloading {service_name}: {e_reload}. Manual intervention might be needed.")

def main():
    check_root_privileges()
    
    sudo_user, user_home, user_uid, user_gid = get_sudo_user_info()
    
    # Resolve the data directory (default ~/.local/share/sgrn)
    default_data_dir = user_home / ".local/share" / "sgrn"
    env_path = default_data_dir / ".env"
    
    env_vars = load_env_file(env_path)
    sgrn_data_dir = Path(env_vars.get("SGRN_DATA_DIR", str(default_data_dir)))
    sgrn_deployment_env = Path(env_vars.get("SGRN_DEPLOYMENT_ENV", f"/home/{sudo_user}/micromamba/envs/SGRN"))
    
    nginx_env_dir = sgrn_deployment_env / "etc" / "nginx"
    systemd_src_dir = sgrn_data_dir / "systemd"
    nginx_src_dir = sgrn_data_dir / "nginx"
    
    # Find all generated .service files
    service_files_to_deploy = []
    if systemd_src_dir.exists():
        for item in systemd_src_dir.rglob("*.service"):
            if item.name == "nginx.service" and item.stat().st_size == 0:
                continue
            service_files_to_deploy.append(item)

    # Find generated nginx configs
    nginx_configs_to_deploy = []
    if nginx_src_dir.exists():
        for item in nginx_src_dir.rglob("*"):
            if item.is_file():
                nginx_configs_to_deploy.append(item)

    print(f"Found {len(service_files_to_deploy)} service files and {len(nginx_configs_to_deploy)} nginx configs to deploy.")

    changed_services = []
    for src_path in service_files_to_deploy:
        dst_path = SYSTEMD_DIR / src_path.name 
        if copy_if_changed(src_path, dst_path, user_uid, user_gid, is_nginx=False):
            changed_services.append(dst_path.name)

    nginx_changed = False
    for src_path in nginx_configs_to_deploy:
        rel_path = src_path.relative_to(nginx_src_dir)
        dst_path = nginx_env_dir / rel_path
        if copy_if_changed(src_path, dst_path, user_uid, user_gid, is_nginx=True):
            nginx_changed = True

    if nginx_changed and "SGRN-nginx.service" not in changed_services:
        changed_services.append("SGRN-nginx.service")

    if changed_services:
        print(f"Detected changes in: {', '.join(changed_services)}")
        reload_systemd_services(changed_services)
    else:
        print("No changes detected. Systemd services are up to date.")

if __name__ == "__main__":
    main()
