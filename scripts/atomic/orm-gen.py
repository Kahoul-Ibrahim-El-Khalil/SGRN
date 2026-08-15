#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent.parent.resolve()
sys.path.append(str(ROOT))

from scripts.db import buildOrm
import dev_env

def main():
    print("🚀 [SGRN] Generating ORM Models...")
    prefix = dev_env.get_conda_prefix("linux")
    buildOrm(ROOT, prefix)
    print("✅ [SGRN] ORM Models generated.")

if __name__ == "__main__":
    main()
