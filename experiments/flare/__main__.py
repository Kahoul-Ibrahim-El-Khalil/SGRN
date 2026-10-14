"""Allow: python3 experiments/flare <command>"""

import sys
from pathlib import Path

_FLARE_DIR = Path(__file__).resolve().parent
if str(_FLARE_DIR) not in sys.path:
    sys.path.insert(0, str(_FLARE_DIR))

from cli import main

if __name__ == "__main__":
    raise SystemExit(main())
