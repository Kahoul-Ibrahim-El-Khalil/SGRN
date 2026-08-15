#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent.parent.resolve()
sys.path.append(str(ROOT))

from scripts.web import buildWeb

def main():
    print("🚀 [SGRN] Building and Deploying Web Application...")
    if buildWeb(ROOT):
        print("✅ [SGRN] Web application deployed successfully.")
    else:
        print("❌ [SGRN] Web application build failed.")
        sys.exit(1)

if __name__ == "__main__":
    main()
