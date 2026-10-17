"""Port-availability helpers shared by demo/experiment launcher scripts.

Uses ``psutil`` (already in the SGRN-WINDOWS/SGRN-LINUX conda envs) to detect
and, optionally, evict whatever process is sitting on a required TCP port.
"""

from __future__ import annotations

import json
import socket
import sys
import time
from pathlib import Path


def read_gateway_port(config_path: Path) -> int:
    """Return the HTTP/WebSocket port declared in a gateway.json file.

    Falls back to 8000 if the config cannot be read or the key is absent.
    """
    try:
        with open(config_path, encoding="utf-8") as fh:
            cfg = json.load(fh)
        return int(cfg["northbound"]["http"]["port"])
    except Exception:
        return 8000


def _is_port_in_use(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def ensure_port_free(port: int, *, kill: bool = True, timeout: float = 5.0) -> None:
    """Guarantee that *port* is not occupied before the gateway starts.

    Parameters
    ----------
    port:
        TCP port to check.
    kill:
        If ``True`` (default), terminate the occupying process automatically.
        If ``False``, raise ``OSError`` when the port is busy.
    timeout:
        Seconds to wait for the port to become free after killing the process.
    """
    if not _is_port_in_use(port):
        return  # nothing to do

    if not kill:
        raise OSError(
            f"Port {port} is already in use. "
            "Stop the conflicting process and try again."
        )

    try:
        import psutil  # type: ignore[import]
    except ImportError:
        raise OSError(
            f"Port {port} is in use and 'psutil' is not installed — "
            "cannot auto-kill. Stop the process manually and retry.\n"
            "  Install: micromamba install -n SGRN-WINDOWS psutil"
        )

    # Find every process listening on the port
    killed_pids: list[int] = []
    for conn in psutil.net_connections(kind="inet"):
        try:
            if conn.laddr.port == port and conn.status in ("LISTEN", "ESTABLISHED"):
                proc = psutil.Process(conn.pid)
                print(
                    f"[warn] Port {port} is in use by '{proc.name()}' "
                    f"(PID {conn.pid}) — terminating it.",
                    file=sys.stderr,
                )
                proc.terminate()
                killed_pids.append(conn.pid)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

    if not killed_pids:
        # Port is in use but we couldn't find the PID (e.g. rapid close race)
        # Just wait a moment and re-check.
        time.sleep(0.5)

    # Wait until the port is actually free (or timeout)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not _is_port_in_use(port):
            return
        time.sleep(0.25)

    # Last-resort: SIGKILL any survivors
    for pid in killed_pids:
        try:
            psutil.Process(pid).kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

    time.sleep(0.5)
    if _is_port_in_use(port):
        raise OSError(
            f"Port {port} is still occupied after attempting to free it. "
            "Please stop the conflicting process manually."
        )
