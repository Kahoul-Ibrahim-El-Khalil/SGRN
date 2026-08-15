import os
import platform as py_platform
from pathlib import Path

# ===========================================================================
# 0. Platform Detection
# ===========================================================================
IS_WINDOWS = py_platform.system() == "Windows"
IS_LINUX   = py_platform.system() == "Linux"

# ===========================================================================
# 1. Paths & Directories
# ===========================================================================
ROOT_DIR         = Path(__file__).parent.parent.resolve()
BUILD_DIR_NAME   = "build"
PREFIX_DIR_NAME  = "prefix"
EXTERN_DIR_NAME  = "extern"
DIST_DIR_NAME    = "dist"
INSTALL_DIR_NAME = "install"

# Sub-components
SGRN_DIR       = ROOT_DIR / "sgrn"
BACKEND_DIR    = SGRN_DIR / "datastore"
CONFIG_DIR     = BACKEND_DIR / "configs"
POSTGRES_DIR   = BACKEND_DIR / "postgres"
ORM_GEN_SCRIPT = BACKEND_DIR / "generate_orm.py"

# Config Subsets
NGINX_CONFIG_DIR   = CONFIG_DIR / "nginx"
SYSTEMD_CONFIG_DIR = CONFIG_DIR / "systemd"
POSTGRES_CONFIG_DIR = CONFIG_DIR / "postgres"

# User Data & Cache
SGRN_USER_HOME = Path.home() / ".sgrn"
PG_DATA_DIR    = SGRN_USER_HOME / "Postgres" / "data"

# Clients
CLIENTS_DIR = ROOT_DIR / "clients"
WEB_CLIENT_DIR = CLIENTS_DIR / "web"
TEMPLATE_TARGET_DIR = SGRN_USER_HOME / "templates"

# Build Cache
WIN64_PKG_CACHE = ROOT_DIR / BUILD_DIR_NAME / "win64" / "pkgcache-msys2"
WIN64_CONDA_CACHE = ROOT_DIR / BUILD_DIR_NAME / "win64" / "pkgcache-conda"

# Environment
CONDA_BIN_SUBDIRS  = ["Library/bin", "bin"]
CONDA_PATH_SUBDIRS = ["Library/bin", "bin", "Scripts"]

# Win64 Environment cleanup
WIN64_CLEARED_ENV_VARS = [
    "CFLAGS", "CXXFLAGS", "LDFLAGS", "CPPFLAGS",
    "CMAKE_PREFIX_PATH", "CMAKE_LIBRARY_PATH", "CMAKE_INCLUDE_PATH",
    "CC", "CXX", "LD", "AR", "NM", "RANLIB",
    "AS", "STRIP", "OBJCOPY", "OBJDUMP",
]

# Documentation / Misc
DESERTATION_FOLDERS = ["Engineering", "Master's", "Technical_Manual"]
TEMPLATE_USER       = "odahim"
TEMPLATE_HOME       = os.environ["HOME"]

# ===========================================================================
# 2. Build Settings
# ===========================================================================
CPU_CORES     = os.cpu_count() or 8
BUILD_TYPE    = os.environ.get("SGRN_BUILD_TYPE", "Debug")
PARALLEL_JOBS = max(1, int(os.environ.get("SGRN_JOBS", CPU_CORES - 1)))
USE_NINJA     = True
USE_PCH       = py_platform.system() != "Windows"

# ===========================================================================
# 3. External Dependencies (URLs & Versions)
# ===========================================================================
DROGON_GIT_URL  = "https://github.com/drogonframework/drogon.git"
DROGON_GIT_TAG  = "v1.9.10"

POSTGREST_VERSION      = "v12.2.0"
POSTGREST_URL_TEMPLATE = (
    "https://github.com/PostgREST/postgrest/releases/download"
    "/{v}/postgrest-{v}-linux-static-x64.tar.xz"
)
MINIO_GIT_URL       = "https://github.com/minio/minio.git"
TIMESCALEDB_GIT_URL = "https://github.com/timescale/timescaledb.git"

MSYS2_UCRT64_REPO = "https://repo.msys2.org/mingw/ucrt64/"

# ===========================================================================
# 4. Target Platforms & Components
# ===========================================================================
PLATFORMS  = ["linux", "win64", "linux-arm64"]
COMPONENTS = ["backend", "s7", "filescrapper"]

CONDA_ENV_NAMES = {
    "win64": ["SGRN-WIN64", "SGRN-WINDOWS", "PFE-WIN64"],
    "linux": ["SGRN"],
    "linux-arm64": ["SGRN-ARM64", "SGRN-ARM", "SGRN-AARCH64"],
}

# ===========================================================================
# 5. Toolchains & Compilers
# ===========================================================================
WIN64_C_COMPILERS   = ["x86_64-w64-mingw32-gcc", "clang", "clang-cl"]
WIN64_CXX_COMPILERS = ["x86_64-w64-mingw32-g++", "clang++", "clang++-22"]
WIN64_RC_COMPILERS  = ["x86_64-w64-mingw32-windres", "llvm-rc"]
CROSS_C_COMPILER    = "clang"
CROSS_CXX_COMPILER  = "clang++"

# ===========================================================================
# 6. Bundling & Distribution
# ===========================================================================
WIN64_THIRDPARTY_DLL_PATTERNS = [
    "libcrypto", "libssl", "libcurl", "zlib", "zstd",
    "fmt", "jsoncpp", "cares", "libssh2",
    "brotli", "idn2", "psl", "iconv", "winpthread", "unistr",
]

MINGW_RUNTIME_DLLS = [
    "libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll", 
    "libatomic-1.dll", "libgomp-1.dll", "libquadmath-0.dll"
]

# ===========================================================================
# 7. System Services & Credentials
# ===========================================================================
MINIO_SERVICE_NAME     = "SGRN-minio.service"
POSTGREST_SERVICE_NAME = "SGRN-postgrest.service"
MINIO_DEFAULT_USER     = "minioadmin"
MINIO_DEFAULT_PASS     = "minioadmin"

DEFAULT_DB_PASSWORD        = "dracaeris"
POSTGRES_PORT              = 5432
POSTGRES_HOST              = "127.0.0.1"
POSTGRES_STARTUP_TIMEOUT   = 30

SGRN_CONFIG_FILES      = ["sgrn.json", "postgrest.conf"]
POSTGRES_CONFIG_FILES  = ["postgresql.conf", "pg_hba.conf"]

# ===========================================================================
# 8. MSYS2 Packages
# ===========================================================================
MSYS2_WIN64_PACKAGES = [
    "mingw-w64-ucrt-x86_64-gcc-libs",
    "mingw-w64-ucrt-x86_64-winpthreads",
    "mingw-w64-ucrt-x86_64-fmt",
    "mingw-w64-ucrt-x86_64-jsoncpp",
    "mingw-w64-ucrt-x86_64-c-ares",
    "mingw-w64-ucrt-x86_64-zstd",
    "mingw-w64-ucrt-x86_64-openssl",
    "mingw-w64-ucrt-x86_64-zlib",
    "mingw-w64-ucrt-x86_64-curl-winssl",
    "mingw-w64-ucrt-x86_64-sqlite3",
    "mingw-w64-ucrt-x86_64-brotli",
    "mingw-w64-ucrt-x86_64-libssh2",
    "mingw-w64-ucrt-x86_64-libidn2",
    "mingw-w64-ucrt-x86_64-libpsl",
    "mingw-w64-ucrt-x86_64-libiconv",
    "mingw-w64-ucrt-x86_64-libunistr",
]
