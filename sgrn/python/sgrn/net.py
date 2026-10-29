"""Network and port-availability utilities for SGRN."""

from __future__ import annotations

import json
import socket
import sys
import time
from pathlib import Path


def read_gateway_port(config_path: Path | str) -> int:
    """Return the HTTP/WebSocket port declared in a gateway.json file.

    Falls back to 8000 if the config cannot be read or the key is absent.
    """
    try:
        with open(config_path, encoding="utf-8") as fh:
            cfg = json.load(fh)
        return int(cfg["northbound"]["http"]["port"])
    except Exception:
        return 8000


def is_port_in_use(port: int) -> bool:
    """Check whether a TCP port is currently open and bound on localhost."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def ensure_port_free(port: int, *, kill: bool = True, timeout: float = 5.0) -> None:
    """Guarantee that *port* is not occupied before starting a server or gateway.

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
    if not is_port_in_use(port):
        return

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
            "cannot auto-kill. Stop the process manually and retry."
        )

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
        time.sleep(0.5)

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not is_port_in_use(port):
            return
        time.sleep(0.25)

    for pid in killed_pids:
        try:
            psutil.Process(pid).kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

    time.sleep(0.5)
    if is_port_in_use(port):
        raise OSError(
            f"Port {port} is still occupied after attempting to free it. "
            "Please stop the conflicting process manually."
        )
