"""Port-availability helpers shared by demo/experiment launcher scripts.

Delegates to the canonical utility implementation in ``sgrn.net``.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_python_sdk = str(ROOT / "sgrn" / "python")
if _python_sdk not in sys.path:
    sys.path.insert(0, _python_sdk)

try:
    from sgrn.net import ensure_port_free, is_port_in_use, read_gateway_port
except ImportError:
    sys.modules.pop("sgrn", None)
    from sgrn.net import ensure_port_free, is_port_in_use, read_gateway_port

_is_port_in_use = is_port_in_use

__all__ = ["ensure_port_free", "is_port_in_use", "_is_port_in_use", "read_gateway_port"]
