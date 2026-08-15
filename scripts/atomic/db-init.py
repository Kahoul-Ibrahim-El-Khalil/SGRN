#!/usr/bin/env python3
import os
import sys
from pathlib import Path

# Add project root to path to import legacy scripts if needed
ROOT = Path(__file__).parent.parent.parent.resolve()
sys.path.append(str(ROOT))

from scripts.db import dbInit
import dev_env

def main():
    print("🚀 [SGRN] Initializing Database...")
    platform = "linux" # DB is typically linux-hosted
    prefix = dev_env.get_conda_prefix(platform)
    
    if dbInit(ROOT, prefix):
        print("✅ [SGRN] Database initialized successfully.")
    else:
        print("❌ [SGRN] Database initialization failed.")
        sys.exit(1)

if __name__ == "__main__":
    main()
