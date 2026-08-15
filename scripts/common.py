import os
import platform as py_platform
import getpass
import shutil
import subprocess
import sys
from pathlib import Path

# ===========================================================================
# Platform Detection
# ===========================================================================

from scripts.config import (
    CONDA_ENV_NAMES, CONDA_BIN_SUBDIRS,
    IS_WINDOWS, IS_LINUX, BUILD_TYPE, PARALLEL_JOBS
)

# ===========================================================================
# Shared Utilities
# ===========================================================================

def fwd(p) -> str:
    return str(p).replace('\\', '/')

def native_path(p) -> str:
    return str(Path(p))

def getCurrentUser():
    sudo_user = os.environ.get("SUDO_USER")
    if sudo_user:
        return sudo_user
    return getpass.getuser()

def getCurrentHome():
    sudo_user = os.environ.get("SUDO_USER")
    if sudo_user:
        # On Linux, we can get the home directory of the sudo user
        import pwd
        return pwd.getpwnam(sudo_user).pw_dir
    return str(Path.home())

def getPlatform(args):
    if "linux-arm64" in args or "linux-arm" in args or "arm64" in args:
        return "linux-arm64"
    if "win64" in args:
        return "win64"
    if "linux" in args:
        return "linux"
    
    # Auto-detection for local host
    import platform as py_platform
    system = py_platform.system()
    machine = py_platform.machine().lower()

    if system == "Windows":
        return "win64"
    if system == "Linux":
        if "arm" in machine or "aarch64" in machine:
            return "linux-arm64"
        return "linux"
    return "linux"

def run(cmd, label="", **kwargs):
    cmd_str = ' '.join(str(c) for c in cmd)
    print(f"  [Run] {label}: {cmd_str}")
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        tag = f"[ERROR] {label}" if label else "[ERROR] command"
        print(f"{tag} failed (exit {result.returncode})\n   cmd: {cmd_str}")
        return False
    return True

def rmtree(path: Path):
    if not path.exists():
        return
    def removeReadonly(func, path, _):
        os.chmod(path, 0o777)
        func(path)
    shutil.rmtree(path, onerror=removeReadonly)

def getCondaPrefix(platform: str) -> str:
    import json
    target_names = list(CONDA_ENV_NAMES.get(platform, ["SGRN-WIN64"]))

    curr_prefix = sys.prefix
    normalized_prefix = curr_prefix.replace("\\", "/")
    for target in target_names:
        if normalized_prefix.endswith(f"/{target}"):
            return curr_prefix

    try:
        res = subprocess.run(
            ["micromamba", "env", "list", "--json"], capture_output=True, text=True
        )
        if res.returncode == 0:
            data = json.loads(res.stdout)
            for env in data.get("envs", []):
                norm_env = env.replace("\\", "/")
                for target_name in target_names:
                    if norm_env.endswith(f"/{target_name}"):
                        return env
    except Exception:
        pass

    return os.environ.get("CONDA_PREFIX", "")

def get_mingw_env(platform_key: str) -> dict[str, str]:
    """Sets up the environment for Windows cross-compilation."""
    env = os.environ.copy()
    conda_prefix = getCondaPrefix(platform_key)
    
    if not conda_prefix:
        print("[Warning] No Conda prefix found for platform:", platform_key)
        return env

    conda_path = Path(conda_prefix)
    
    # Toolchain paths
    cc  = conda_path / "Library" / "bin" / "x86_64-w64-mingw32-gcc.exe"
    cxx = conda_path / "Library" / "bin" / "x86_64-w64-mingw32-g++.exe"
    rc  = conda_path / "Library" / "bin" / "x86_64-w64-mingw32-windres.exe"
    
    if cc.exists():  env["SGRN_WIN64_CC"]  = fwd(cc)
    if cxx.exists(): env["SGRN_WIN64_CXX"] = fwd(cxx)
    if rc.exists():  env["SGRN_WIN64_RC"]  = fwd(rc)
    
    env["SGRN_WIN64_PREFIX"] = fwd(conda_path)
    
    # MSYS2 Root for UCRT libs
    msys_root = conda_path / "x86_64-w64-mingw32" / "sysroot" / "ucrt64"
    if msys_root.exists():
        env["SGRN_WIN64_MSYS2_ROOT"] = fwd(msys_root)

    # Path setup: Put Conda Library/bin first
    path_entries = env.get("PATH", "").split(os.pathsep)
    path_entries.insert(0, str(conda_path / "Library" / "bin"))
    path_entries.insert(0, str(conda_path / "bin"))
    env["PATH"] = os.pathsep.join(dict.fromkeys(path_entries))
    
    return env

def run_cmake(source_dir: Path, build_dir: Path, flags: list[str], env: dict[str, str]):
    """Standardized CMake run sequence: Config -> Build -> Install."""
    source_dir = Path(source_dir).absolute()
    build_dir = Path(build_dir).absolute()
    
    # 1. Configure
    config_cmd = [
        "cmake", "-G", "Ninja",
        "-S", fwd(source_dir),
        "-B", fwd(build_dir),
        *flags
    ]
    if not run(config_cmd, label=f"Config {source_dir.name}", env=env):
        sys.exit(1)
        
    # 2. Build
    build_cmd = ["cmake", "--build", fwd(build_dir), "--parallel", str(PARALLEL_JOBS)]
    if not run(build_cmd, label=f"Build {source_dir.name}", env=env):
        sys.exit(1)
        
    # 3. Install (to local prefix)
    install_cmd = ["cmake", "--install", fwd(build_dir)]
    if not run(install_cmd, label=f"Install {source_dir.name}", env=env):
        sys.exit(1)
