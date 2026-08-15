#!/usr/bin/env python3
import sys
import shutil
from pathlib import Path

ROOT = Path(__file__).parent.parent.parent.resolve()
sys.path.append(str(ROOT))

from scripts.db import dbInit
from scripts.config import PG_DATA_DIR
import dev_env

def main():
    print("⚠️ [SGRN] RE-INITIALIZING DATABASE (Destructive!)...")
    
    if PG_DATA_DIR.exists():
        print(f"🗑️  Removing existing data at {PG_DATA_DIR}...")
        shutil.rmtree(PG_DATA_DIR)
    
    prefix = dev_env.get_conda_prefix("linux")
    if dbInit(ROOT, prefix):
        print("✅ [SGRN] Database re-initialized.")
    else:
        print("❌ [SGRN] Database re-initialization failed.")
        sys.exit(1)

if __name__ == "__main__":
    main()
