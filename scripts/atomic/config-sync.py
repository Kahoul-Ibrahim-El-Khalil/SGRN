#!/usr/bin/env python3
import sys
import argparse
from pathlib import Path

ROOT = Path(__file__).parent.parent.parent.resolve()
sys.path.append(str(ROOT))

from scripts.deploy import syncConfigs, deploySystemd
import dev_env

def main():
    parser = argparse.ArgumentParser(description="Sync SGRN configurations")
    parser.add_argument("--platform", choices=["linux", "win64"], default="linux")
    parser.add_argument("--systemd", action="store_true", help="Also deploy systemd services (requires sudo)")
    args = parser.parse_args()

    print(f"🚀 [SGRN] Syncing Configurations for {args.platform}...")
    prefix = dev_env.get_conda_prefix(args.platform)
    
    syncConfigs(ROOT, prefix)
    
    if args.systemd:
        deploySystemd(ROOT)
        
    print("✅ [SGRN] Configurations synced.")

if __name__ == "__main__":
    main()
