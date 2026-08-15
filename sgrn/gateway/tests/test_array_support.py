#!/usr/bin/env python3
"""
test_array_support.py — Integration test for S7 Gateway array support.

Tests:
  1. HTTP: POST full array   → 200, value returned
  2. HTTP: GET full array    → returns JSON array
  3. HTTP: GET element [2]   → returns scalar at index 2
  4. HTTP: POST to element [2] → updates only that element
  5. HTTP: GET full array    → only index 2 changed
  6. OPC UA (optional): read the same array variable via OPC UA

Usage:
  python3 test_array_support.py \\
      --host 127.0.0.1 \\
      --http-port 8080 \\
      [--opcua-port 4840] \\
      [--db DB2] \\
      [--field temperatures] \\
      [--array-size 10]

Requirements:
  pip install requests
  pip install asyncua   # optional, for OPC UA test
"""

import argparse
import json
import sys
import time

try:
    import requests
except ImportError:
    print("[FATAL] 'requests' package not found. Install with: pip install requests", file=sys.stderr)
    sys.exit(1)

OPCUA_AVAILABLE = False
try:
    import asyncio
    import asyncua  # noqa: F401
    OPCUA_AVAILABLE = True
except ImportError:
    pass


# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def ok(msg: str):
    print(f"  [PASS] {msg}")


def fail(msg: str):
    print(f"  [FAIL] {msg}", file=sys.stderr)
    sys.exit(1)


def http_get(base: str, path: str) -> requests.Response:
    url = f"{base}{path}"
    resp = requests.get(url, timeout=5)
    return resp


def http_post(base: str, path: str, body) -> requests.Response:
    url = f"{base}{path}"
    resp = requests.post(url, json=body, timeout=5)
    return resp


# ─────────────────────────────────────────────────────────────────────────────
# Test cases
# ─────────────────────────────────────────────────────────────────────────────

def run_http_tests(base: str, db: str, field: str, size: int):
    array_path = f"/data/{db}/{field}"
    elem2_path = f"/data/{db}/{field}/2"

    print(f"\n{'='*60}")
    print(f"  HTTP Array Tests  |  {base}{array_path}")
    print(f"{'='*60}")

    # ── Test 1: POST full array ──────────────────────────────────────────────
    print("\n[1] POST full array")
    initial_array = [float(i * 10) for i in range(size)]
    r = http_post(base, array_path, initial_array)
    if r.status_code != 200:
        fail(f"Expected 200, got {r.status_code}: {r.text}")
    body = r.json()
    ok(f"POST full array → 200. Response: {json.dumps(body)[:120]}")

    # Small delay to allow arena to commit
    time.sleep(0.1)

    # ── Test 2: GET full array ────────────────────────────────────────────────
    print("\n[2] GET full array")
    r = http_get(base, array_path)
    if r.status_code != 200:
        fail(f"Expected 200, got {r.status_code}: {r.text}")
    arr = r.json()
    if not isinstance(arr, list):
        fail(f"Expected a JSON array, got: {type(arr).__name__}")
    if len(arr) != size:
        fail(f"Expected array of size {size}, got {len(arr)}")
    ok(f"GET full array → {arr[:5]}{'...' if size > 5 else ''}")

    # ── Test 3: GET single element at index 2 ────────────────────────────────
    print("\n[3] GET array element [2]")
    r = http_get(base, elem2_path)
    if r.status_code != 200:
        fail(f"Expected 200, got {r.status_code}: {r.text}")
    elem = r.json()
    expected_elem = initial_array[2]
    if abs(float(elem) - expected_elem) > 1e-6:
        fail(f"Element [2] should be {expected_elem}, got {elem}")
    ok(f"GET element [2] → {elem}  (expected {expected_elem})")

    # ── Test 4: POST scalar to index 2 ───────────────────────────────────────
    print("\n[4] POST scalar to element [2]")
    new_value = 999.0
    r = http_post(base, elem2_path, new_value)
    if r.status_code != 200:
        fail(f"Expected 200, got {r.status_code}: {r.text}")
    body = r.json()
    if "value" not in body:
        fail(f"Response missing 'value' key: {body}")
    if abs(float(body["value"]) - new_value) > 1e-6:
        fail(f"Response value should be {new_value}, got {body['value']}")
    ok(f"POST element [2] = {new_value} → 200, value={body['value']}")

    time.sleep(0.1)

    # ── Test 5: GET full array, only index 2 should have changed ─────────────
    print("\n[5] GET full array — verify only index 2 changed")
    r = http_get(base, array_path)
    if r.status_code != 200:
        fail(f"Expected 200, got {r.status_code}: {r.text}")
    arr2 = r.json()
    if not isinstance(arr2, list):
        fail(f"Expected a JSON array, got: {type(arr2).__name__}")
    for i, (orig, updated) in enumerate(zip(initial_array, arr2)):
        if i == 2:
            if abs(float(updated) - new_value) > 1e-6:
                fail(f"Index 2 should be {new_value}, got {updated}")
        else:
            if abs(float(updated) - orig) > 1e-6:
                fail(f"Index {i} should be unchanged ({orig}), got {updated}")
    ok(f"Full array after indexed write: only index 2 = {arr2[2]}")

    # ── Test 6: Out-of-bounds index → 416 ─────────────────────────────────────
    print("\n[6] GET out-of-bounds index → expect 416")
    r = http_get(base, f"/data/{db}/{field}/{size + 100}")
    if r.status_code not in (404, 416):
        fail(f"Expected 404 or 416 for out-of-bounds, got {r.status_code}")
    ok(f"Out-of-bounds GET → {r.status_code} (as expected)")

    # ── Test 7: POST object to indexed path → 400 ────────────────────────────
    print("\n[7] POST JSON object to indexed path → expect 400")
    r = http_post(base, elem2_path, {"bad": "value"})
    if r.status_code != 400:
        fail(f"Expected 400, got {r.status_code}: {r.text}")
    ok(f"POST object to indexed path → 400 (correct rejection)")

    print(f"\n{'='*60}")
    print("  All HTTP array tests PASSED")
    print(f"{'='*60}\n")


def run_opcua_tests(host: str, port: int, db: str, field: str, size: int):
    """Optional OPC UA test using asyncua."""
    import asyncio
    from asyncua import Client

    array_node_id = f"ns=1;s={db}.{field}"

    async def _test():
        print(f"\n{'='*60}")
        print(f"  OPC UA Array Tests  |  opc.tcp://{host}:{port}")
        print(f"{'='*60}")

        async with Client(url=f"opc.tcp://{host}:{port}") as client:
            # Discover node
            node = client.get_node(array_node_id)
            try:
                val = await node.read_value()
            except Exception as exc:
                fail(f"OPC UA read failed: {exc}")

            print(f"\n[OPC UA 1] Read array node '{array_node_id}'")
            if not hasattr(val, '__iter__'):
                fail(f"Expected an iterable array value, got: {type(val).__name__}")
            val_list = list(val)
            if len(val_list) != size:
                fail(f"Expected array of size {size}, got {len(val_list)}")
            ok(f"Read array: {val_list[:5]}{'...' if size > 5 else ''}")

            # Write a new array
            print(f"\n[OPC UA 2] Write new array")
            from asyncua.ua import Variant, VariantType
            new_arr = [float(i + 100) for i in range(size)]
            v = Variant(new_arr, VariantType.Double)
            try:
                await node.write_value(v)
            except Exception as exc:
                fail(f"OPC UA write failed: {exc}")
            ok(f"Write array succeeded: {new_arr[:5]}...")

            # Read back and verify
            print(f"\n[OPC UA 3] Read back and verify")
            val2 = await node.read_value()
            val2_list = list(val2)
            for i, (expected, actual) in enumerate(zip(new_arr, val2_list)):
                if abs(float(actual) - expected) > 1e-5:
                    fail(f"Mismatch at index {i}: expected {expected}, got {actual}")
            ok(f"Read-back matches written array.")

        print(f"\n{'='*60}")
        print("  All OPC UA array tests PASSED")
        print(f"{'='*60}\n")

    asyncio.run(_test())


# ─────────────────────────────────────────────────────────────────────────────
# CLI entry point
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="S7 Gateway — Array support integration test")
    parser.add_argument("--host", default="127.0.0.1", help="Gateway host (default: 127.0.0.1)")
    parser.add_argument("--http-port", type=int, default=8080, help="HTTP port (default: 8080)")
    parser.add_argument("--opcua-port", type=int, default=4840, help="OPC UA port (default: 4840)")
    parser.add_argument("--db", default="DB2", help="DB name as registered in the schema (default: DB2)")
    parser.add_argument("--field", default="temperatures", help="Array field name (default: temperatures)")
    parser.add_argument("--array-size", type=int, default=10, help="Expected array size (default: 10)")
    parser.add_argument("--no-opcua", action="store_true", help="Skip OPC UA tests even if asyncua is available")
    args = parser.parse_args()

    base_url = f"http://{args.host}:{args.http_port}"

    # Verify gateway is reachable
    try:
        requests.get(f"{base_url}/endpoints", timeout=3)
    except requests.exceptions.ConnectionError:
        print(f"[ERROR] Cannot connect to gateway at {base_url}. Is it running?", file=sys.stderr)
        sys.exit(1)

    run_http_tests(base_url, args.db, args.field, args.array_size)

    if OPCUA_AVAILABLE and not args.no_opcua:
        run_opcua_tests(args.host, args.opcua_port, args.db, args.field, args.array_size)
    elif not args.no_opcua:
        print("[INFO] asyncua not installed — skipping OPC UA tests. (pip install asyncua)")

    print("[DONE] All tests passed.")


if __name__ == "__main__":
    main()
