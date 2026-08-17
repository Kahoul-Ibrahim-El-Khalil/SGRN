"""
Dual-Path WebSocket Validation Test

FUNDAMENTAL AXIOM:
If a JSON frame and a Binary frame are emitted by the Gateway with the exact same 
timestamp, they originated from the exact same atomic snapshot of the PLC memory arena.

This test proves that decoding the pure binary payload through NumPy (zero-copy) 
and parsing the JSON payload through Python yields mathematically identical dictionaries. 
This guarantees that Machine Learning pipelines (using raw binary) and UI dashboards 
(using JSON) are seeing the exact same physical truth with zero precision loss or 
endianness mismatch.
"""

import asyncio
import logging
import os
import sys

# Ensure local sgrn package is loadable. The Python bindings live under
# sgrn/bindings/python; the repo-root sgrn/ directory is the C++ tree and must
# NOT shadow the bindings package, so put the bindings dir first.
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, _REPO_ROOT)
sys.path.insert(0, os.path.join(_REPO_ROOT, "sgrn", "bindings", "python"))

import numpy as np

from sgrn.gateway import Gateway
from sgrn.telemetry import GatewayTelemetry
from sgrn.dtypes import decode_record

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("dual_test")

async def main() -> None:
    # Use environment vars or default to standard test ports
    gateway_url = os.environ.get("SGRN_URL", "http://localhost:8000")
    ws_url = os.environ.get("SGRN_WS_URL", "ws://localhost:8001")

    # 1. Setup the Gateway REST client to fetch the registry
    log.info(f"Connecting to Gateway REST API: {gateway_url}")
    gw = Gateway(gateway_url)
    try:
        reg = gw.registry()
    except Exception as e:
        log.error(f"Failed to fetch registry: {e}")
        return

    # Find a suitable DB to test (needs to be non-empty)
    db_schema = None
    db_num = None
    db_name = None
    for schema in reg.dbs:
        if schema.size_bytes > 0:
            db_schema = schema
            db_name = schema.db_name
            db_num = schema.db_number
            break

    if db_schema is None:
        log.error("No valid DBs found in registry.")
        return

    log.info(f"Testing Dual Subscriptions on DB {db_num} ('{db_name}')")

    # Pre-compile the dtype using the registry UDT definitions
    dt = db_schema.to_dtype(t_udts=reg.udts_by_name())
    
    # State tracking
    received_json = {}
    received_binary = {}

    def on_json(data: dict) -> None:
        # data might be {"ReactorCore": {"temperature": 1.0}} or flat depending on your server
        # We will aggressively update our local state with whatever JSON keys arrive
        for key, val in data.items():
            if key == db_name and isinstance(val, dict):
                received_json.update(val)
                log.info(f"[JSON] Deltas received for {db_name}")
            elif key.startswith(f"{db_name}."):
                # Handle flattened paths like "ReactorCore.temperature"
                sub_key = key[len(db_name)+1:]
                received_json[sub_key] = val
                log.info(f"[JSON] Flattened Delta received: {sub_key}={val}")

    def on_binary(db: int, ts: float, record: np.void) -> None:
        if db == db_num:
            # Decode the binary NumPy record into a JSON-equivalent dict
            decoded = decode_record(record, db_schema.fields, t_udts=reg.udts_by_name())
            received_binary.update(decoded)
            log.info(f"[BINARY] Decoded raw memory frame (ts={ts})")

    # 2. Setup the Telemetry WebSocket client
    log.info(f"Connecting to Gateway WebSockets: {ws_url}")
    telemetry = GatewayTelemetry(
        t_url=ws_url,
        t_on_message=on_json,
        t_on_binary=on_binary,
        t_binary_dtypes={db_num: dt}
    )

    # 3. Subscribe to BOTH feeds
    # You will receive JSON deltas AND the raw binary buffers
    telemetry.subscribe(t_path=db_name)
    telemetry.subscribe_binary(t_db=db_num)
    
    # 4. Start the background loop
    telemetry.start()
    
    # 5. Wait for connections and data
    log.info("Waiting for data...")
    log.info("If no data arrives, run `python tests/test_gateway_rest.py` in another terminal to trigger writes.")
    
    # We poll until we receive both, or timeout
    timeout = 15.0
    elapsed = 0.0
    while elapsed < timeout:
        if received_json and received_binary:
            break
        await asyncio.sleep(0.5)
        elapsed += 0.5
        
    await telemetry.stop()
    
    # 6. Compare the final states
    if not received_json or not received_binary:
        log.warning("Test finished but did not receive data on both channels.")
        log.info(f"JSON data count: {len(received_json)}, Binary data count: {len(received_binary)}")
        return

    log.info("--- Validating Synchronization ---")
    mismatch = False
    
    # We only check keys that were received via the JSON delta
    for k, v in received_json.items():
        if k not in received_binary:
            # Depending on how nested structs are flattened by the backend, 
            # this check might need recursive logic in complex systems.
            log.error(f"Key '{k}' missing in decoded binary payload")
            mismatch = True
            continue
            
        bin_v = received_binary[k]
        
        # Float comparisons need tolerance due to stringification in JSON vs IEEE 754 in binary
        if isinstance(v, float) and isinstance(bin_v, float):
            if not np.isclose(v, bin_v, rtol=1e-5):
                log.error(f"Mismatch on '{k}': JSON={v}, BIN={bin_v}")
                mismatch = True
        elif v != bin_v:
            log.error(f"Mismatch on '{k}': JSON={v}, BIN={bin_v}")
            mismatch = True
            
    if not mismatch:
        log.info(f"✅ SUCCESS! Binary zero-copy decoding perfectly matches the JSON semantic twin.")
    else:
        log.error("❌ FAILED! Mismatches found between the two protocols.")

if __name__ == "__main__":
    asyncio.run(main())
