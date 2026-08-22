#!/usr/bin/env python3
"""
test_types.py — Full-coverage OPC UA ↔ HTTP read/write test for the types simulation.

Exercises every S7 data-type path through the gateway:
  Primitives, Strings, Time/Date, Enums (known-buggy — tracked separately),
  Scalar arrays, String arrays, Structs (leaf-write + struct-read via HTTP),
  Regression controls, Full DB snapshot.

Usage:
    # Start the gateway first:
    #   ./sgrn-gateway ./sgrn/gateway/simulations/types/gateway.json
    python3 sgrn/gateway/simulations/types/test_types.py
    python3 sgrn/gateway/simulations/types/test_types.py --opcua opc.tcp://localhost:4840 --http http://localhost:8000

Dependencies:
    pip install asyncua requests
"""

import argparse
import asyncio
import sys
import datetime
import requests
from asyncua import Client, ua

# ─────────────────────────────────────────────────────────────────────────────
# Globals
# ─────────────────────────────────────────────────────────────────────────────

DB_NODE   = "TypeCoverage"   # top-level browse name (matches DATA_BLOCK name)
RESULTS   = []               # (name, ok, detail)
HTTP_BASE = "http://localhost:8000"


def record(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append((name, ok, detail))
    status = "PASS" if ok else "FAIL"
    suffix = f" — {detail}" if detail and not ok else ""
    print(f"[{status}] {name}{suffix}")


def record_warn(name: str, ok: bool, detail: str = "") -> None:
    """Like record() but marks failures as WARN (known issues, non-fatal)."""
    RESULTS.append((f"[ENUM]{name}", ok, detail))
    status = "PASS" if ok else "WARN"
    suffix = f" — {detail}" if detail else ""
    print(f"[{status}] {name}{suffix}")


# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

async def find_node(client: Client, *path: str):
    """Resolve a node under Objects/TypeCoverage by browse names."""
    node = client.nodes.objects
    node = await node.get_child(f"1:{DB_NODE}")
    for name in path:
        node = await node.get_child(f"1:{name}")
    return node


async def write_node(client: Client, path: list, value, ua_type) -> None:
    node = await find_node(client, *path)
    await node.write_value(ua.Variant(value, ua_type))


async def read_node(client: Client, path: list):
    node = await find_node(client, *path)
    return await node.read_value()


def http_get(field_path: str):
    """GET /data/TypeCoverage/{field_path} — returns parsed JSON."""
    r = requests.get(f"{HTTP_BASE}/data/{DB_NODE}/{field_path}", timeout=5)
    r.raise_for_status()
    return r.json()


def http_get_db():
    """GET /data/TypeCoverage — full DB snapshot."""
    r = requests.get(f"{HTTP_BASE}/data/{DB_NODE}", timeout=5)
    r.raise_for_status()
    return r.json()


def approx(a, b, tol=1e-4) -> bool:
    try:
        return abs(float(a) - float(b)) < tol
    except (TypeError, ValueError):
        return a == b


# ─────────────────────────────────────────────────────────────────────────────
# §1  Scalar primitives
# ─────────────────────────────────────────────────────────────────────────────

async def test_scalars(c: Client) -> None:
    print("\n── §1 Scalar primitives ──────────────────────────────────────────")

    cases = [
        # (browse_name,  http_path,   write_value,      ua_type,                expected_http)
        ("p_bool",  "p_bool",  True,            ua.VariantType.Boolean, True),
        ("p_byte",  "p_byte",  200,             ua.VariantType.Byte,    200),
        ("p_usint", "p_usint", 123,             ua.VariantType.Byte,    123),
        ("p_sint",  "p_sint",  -42,             ua.VariantType.SByte,   -42),
        ("p_word",  "p_word",  60000,           ua.VariantType.UInt16,  60000),
        ("p_uint",  "p_uint",  50000,           ua.VariantType.UInt16,  50000),
        ("p_int",   "p_int",   -1000,           ua.VariantType.Int16,   -1000),
        ("p_dword", "p_dword", 3_000_000_000,   ua.VariantType.UInt32,  3_000_000_000),
        ("p_udint", "p_udint", 4_000_000_000,   ua.VariantType.UInt32,  4_000_000_000),
        ("p_dint",  "p_dint",  -1_500_000,      ua.VariantType.Int32,   -1_500_000),
        ("p_lint",  "p_lint",  -9_000_000_000,  ua.VariantType.Int64,   -9_000_000_000),
        ("p_ulint", "p_ulint", 18_000_000_000,  ua.VariantType.UInt64,  18_000_000_000),
        ("p_real",  "p_real",  3.14,            ua.VariantType.Float,   None),
        ("p_lreal", "p_lreal", 2.718281828,     ua.VariantType.Double,  None),
    ]

    for bname, hpath, wval, utype, http_exp in cases:
        await write_node(c, [bname], wval, utype)
        got_ua = await read_node(c, [bname])
        ok_ua = approx(got_ua, wval) if isinstance(wval, float) else got_ua == wval
        record(f"ua:  {bname} = {wval!r}", ok_ua, f"got {got_ua!r}")

        got_http = http_get(hpath)
        if http_exp is not None:
            record(f"http:{hpath} = {http_exp!r}", got_http == http_exp, f"got {got_http!r}")
        else:
            record(f"http:{hpath} approx {wval!r}", approx(got_http, wval, 1e-3), f"got {got_http!r}")

    # Char / WChar
    await write_node(c, ["p_char"],  "A", ua.VariantType.String)
    record("http:p_char = 'A'", http_get("p_char") == "A")
    await write_node(c, ["p_wchar"], "E", ua.VariantType.String)
    record("http:p_wchar = 'E'", http_get("p_wchar") == "E")


# ─────────────────────────────────────────────────────────────────────────────
# §2  String types
# ─────────────────────────────────────────────────────────────────────────────

async def test_strings(c: Client) -> None:
    print("\n── §2 String types ───────────────────────────────────────────────")

    cases = [
        ("s_string",    "Hello, SGRN!"),
        ("s_wstring",   "Unicode"),
        ("s_short_str", "Hi"),
    ]
    for field, val in cases:
        await write_node(c, [field], val, ua.VariantType.String)
        got_ua   = await read_node(c, [field])
        got_http = http_get(field)
        record(f"ua:  {field}", got_ua == val,   f"got {got_ua!r}")
        record(f"http:{field}", got_http == val, f"got {got_http!r}")


# ─────────────────────────────────────────────────────────────────────────────
# §3  Time / date types
# ─────────────────────────────────────────────────────────────────────────────

async def test_time_types(c: Client) -> None:
    print("\n── §3 Time / date types ──────────────────────────────────────────")

    # DTL — write DateTime via OPC UA, verify both OPC UA read-back and HTTP
    dt_write = datetime.datetime(2000, 1, 1, 12, 30, 45, tzinfo=datetime.timezone.utc)
    await write_node(c, ["t_dtl"], dt_write, ua.VariantType.DateTime)
    await asyncio.sleep(0.08)

    got_dt = await read_node(c, ["t_dtl"])
    if hasattr(got_dt, "tzinfo") and got_dt.tzinfo:
        got_utc = got_dt.astimezone(datetime.timezone.utc)
    else:
        got_utc = got_dt
    ok_dt = (got_utc.year == 2000 and got_utc.month == 1 and got_utc.day == 1
             and got_utc.hour == 12 and got_utc.minute == 30 and got_utc.second == 45)
    record("ua:  t_dtl 2000-01-01 12:30:45 UTC", ok_dt, f"got {got_utc}")

    http_dtl = http_get("t_dtl")
    record("http:t_dtl not empty", bool(http_dtl), f"got {http_dtl!r}")
    if isinstance(http_dtl, str):
        record("http:t_dtl date=2000-01-01", "2000-01-01" in http_dtl, f"got {http_dtl!r}")
    elif isinstance(http_dtl, dict):
        record("http:t_dtl year=2000", http_dtl.get("year") == 2000, f"got {http_dtl!r}")

    # TIME field (milliseconds as Int32)
    await write_node(c, ["t_time"], 5000, ua.VariantType.Int32)
    await asyncio.sleep(0.05)
    got_time = http_get("t_time")
    record("http:t_time = 5000 ms", approx(got_time, 5000, 1), f"got {got_time!r}")


# ─────────────────────────────────────────────────────────────────────────────
# §4  Enums  (known-buggy — WARN on failure, non-fatal)
# ─────────────────────────────────────────────────────────────────────────────

async def test_enums(c: Client) -> None:
    print("\n── §4 Enums  [WARN only — known issues, non-fatal] ───────────────")

    scalar_cases = [
        # (browse_name,    http_path,       write_val, description)
        ("e_status",      "e_status",         1,   "USInt RUNNING=1"),
        ("e_plant_mode",  "e_plant_mode",     2,   "native TIA RUNNING=2"),
        ("e_direction",   "e_direction",     -1,   "SInt BACKWARD=-1"),
        ("e_error_code",  "e_error_code",  -100,   "DInt ERR_COMM=-100"),
        ("e_level",       "e_level",        255,   "Byte HIGH=255 boundary"),
        ("e_alarm_class", "e_alarm_class",   20,   "UInt sparse WARN=20"),
        ("e_inline_prio", "e_inline_prio",    2,   "inline #ENUM HIGH=2"),
    ]

    for bname, hpath, wval, desc in scalar_cases:
        try:
            await write_node(c, [bname], wval, ua.VariantType.Int32)
            got_ua   = await read_node(c, [bname])
            got_http = http_get(hpath)
            record_warn(f"ua:  {bname}={wval} ({desc})", int(got_ua) == wval, f"got {got_ua!r}")
            record_warn(f"http:{hpath}={wval}", got_http == wval, f"got {got_http!r}")
        except Exception as e:
            record_warn(f"enum {bname}", False, str(e))

    # Rejection: undeclared members must be rejected or leave value unchanged
    for bname, wval, desc in [("e_status", 99, "undeclared"),
                               ("e_alarm_class", 500, "in-range but not declared")]:
        before = await read_node(c, [bname])
        try:
            await write_node(c, [bname], wval, ua.VariantType.Int32)
            after = await read_node(c, [bname])
            record_warn(f"reject {bname}={wval} ({desc})", after == before,
                        "accepted silently" if after != before else "value unchanged (ok)")
        except ua.UaStatusCodeError as e:
            record_warn(f"reject {bname}={wval} ({desc})", True, f"correctly rejected: {e}")

    # Array of enums
    arr = [0, 1, 2, 3]
    try:
        await write_node(c, ["a_status"], arr, ua.VariantType.Int32)
        await asyncio.sleep(0.06)
        got = await read_node(c, ["a_status"])
        got_list = list(got) if got is not None else []
        record_warn("ua:  a_status[4] round-trip", got_list == arr, f"got {got_list!r}")
    except Exception as e:
        record_warn("ua:  a_status[4]", False, str(e))


# ─────────────────────────────────────────────────────────────────────────────
# §5  Scalar arrays
# ─────────────────────────────────────────────────────────────────────────────

async def test_scalar_arrays(c: Client) -> None:
    print("\n── §5 Scalar arrays ──────────────────────────────────────────────")

    cases = [
        ("a_bool",  [True, False, True, False, True, False, True, False], ua.VariantType.Boolean),
        ("a_byte",  [0, 1, 127, 255, 10, 20, 30, 40],                    ua.VariantType.Byte),
        ("a_int",   [-1000, 0, 1000, 32767],                             ua.VariantType.Int16),
        ("a_dint",  [-2_000_000, 0, 2_000_000, -1],                      ua.VariantType.Int32),
        ("a_real",  [0.0, 1.5, -1.5, 3.14],                              ua.VariantType.Float),
        ("a_lreal", [2.718281828, -1.0],                                  ua.VariantType.Double),
    ]

    for field, wval, utype in cases:
        await write_node(c, [field], wval, utype)
        await asyncio.sleep(0.06)
        got_ua   = await read_node(c, [field])
        lst_ua   = list(got_ua) if got_ua is not None else []
        ok_ua    = len(lst_ua) == len(wval) and all(approx(a, b, 1e-3) for a, b in zip(lst_ua, wval))
        record(f"ua:  {field}[{len(wval)}]", ok_ua, f"got {lst_ua!r}")
        got_http = http_get(field)
        ok_http  = (isinstance(got_http, list) and len(got_http) == len(wval)
                    and all(approx(a, b, 1e-3) for a, b in zip(got_http, wval)))
        record(f"http:{field}[{len(wval)}]", ok_http, f"got {got_http!r}")


# ─────────────────────────────────────────────────────────────────────────────
# §6  String arrays
# ─────────────────────────────────────────────────────────────────────────────

async def test_string_arrays(c: Client) -> None:
    print("\n── §6 String arrays ──────────────────────────────────────────────")

    val = ["Alpha", "Beta", "Gamma", "Delta"]
    await write_node(c, ["a_strings"], val, ua.VariantType.String)
    await asyncio.sleep(0.06)
    got = http_get("a_strings")
    record("http:a_strings[4]", isinstance(got, list) and got == val, f"got {got!r}")


# ─────────────────────────────────────────────────────────────────────────────
# §7  Struct fields — write leaves via OPC UA, read struct via HTTP
# ─────────────────────────────────────────────────────────────────────────────

async def test_structs(c: Client) -> None:
    print("\n── §7 Struct fields ──────────────────────────────────────────────")

    await write_node(c, ["st_prims", "b_bool"],  True,   ua.VariantType.Boolean)
    await write_node(c, ["st_prims", "b_int"],   -999,   ua.VariantType.Int16)
    await write_node(c, ["st_prims", "b_real"],  2.71,   ua.VariantType.Float)
    await write_node(c, ["st_prims", "b_dint"],  123456, ua.VariantType.Int32)
    await asyncio.sleep(0.08)

    st = http_get("st_prims")
    record("http:st_prims is object",       isinstance(st, dict))
    record("http:st_prims.b_bool = true",   st.get("b_bool") is True,              f"got {st.get('b_bool')!r}")
    record("http:st_prims.b_int = -999",    st.get("b_int") == -999,               f"got {st.get('b_int')!r}")
    record("http:st_prims.b_dint = 123456", st.get("b_dint") == 123456,            f"got {st.get('b_dint')!r}")
    record("http:st_prims.b_real approx 2.71", approx(st.get("b_real", 0), 2.71, 0.01), f"got {st.get('b_real')!r}")

    got_int = await read_node(c, ["st_prims", "b_int"])
    record("ua:  st_prims.b_int = -999", got_int == -999, f"got {got_int!r}")

    await write_node(c, ["st_motor", "name"],     "TurboPump1", ua.VariantType.String)
    await write_node(c, ["st_motor", "running"],  True,         ua.VariantType.Boolean)
    await write_node(c, ["st_motor", "setpoint"], 75.5,         ua.VariantType.Float)
    await asyncio.sleep(0.08)

    motor = http_get("st_motor")
    record("http:st_motor.name = TurboPump1",  motor.get("name") == "TurboPump1",              f"got {motor.get('name')!r}")
    record("http:st_motor.running = true",     motor.get("running") is True,                    f"got {motor.get('running')!r}")
    record("http:st_motor.setpoint approx 75.5", approx(motor.get("setpoint", 0), 75.5, 0.1),  f"got {motor.get('setpoint')!r}")


# ─────────────────────────────────────────────────────────────────────────────
# §8  Regression controls
# ─────────────────────────────────────────────────────────────────────────────

async def test_regression(c: Client) -> None:
    print("\n── §8 Regression controls ────────────────────────────────────────")

    await write_node(c, ["ctrl_counter"], 424242,       ua.VariantType.Int32)
    await write_node(c, ["ctrl_flag"],    False,         ua.VariantType.Boolean)
    await write_node(c, ["ctrl_label"],   "regression", ua.VariantType.String)
    await asyncio.sleep(0.06)

    record("ua:  ctrl_counter = 424242",   await read_node(c, ["ctrl_counter"]) == 424242)
    record("ua:  ctrl_flag = false",       await read_node(c, ["ctrl_flag"]) is False)
    record("ua:  ctrl_label = regression", await read_node(c, ["ctrl_label"]) == "regression")
    record("http:ctrl_counter = 424242",   http_get("ctrl_counter") == 424242)
    record("http:ctrl_flag = false",       http_get("ctrl_flag") is False)
    record("http:ctrl_label = regression", http_get("ctrl_label") == "regression")


# ─────────────────────────────────────────────────────────────────────────────
# §9  Full DB snapshot
# ─────────────────────────────────────────────────────────────────────────────

async def test_snapshot(_c: Client) -> None:
    print("\n── §9 Full DB snapshot ───────────────────────────────────────────")

    snap = http_get_db()
    record("GET /data/TypeCoverage is object", isinstance(snap, dict))
    for key in ["p_bool", "p_int", "p_real", "p_lreal", "s_string", "s_wstring",
                "t_dtl", "t_time", "e_status", "e_plant_mode",
                "a_bool", "a_int", "a_real", "a_strings",
                "st_prims", "st_motor", "ctrl_counter", "ctrl_flag"]:
        record(f"  snapshot has '{key}'", key in snap)


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

async def run(opcua_url: str) -> None:
    async with Client(url=opcua_url) as client:
        await test_scalars(client)
        await test_strings(client)
        await test_time_types(client)
        await test_enums(client)
        await test_scalar_arrays(client)
        await test_string_arrays(client)
        await test_structs(client)
        await test_regression(client)
        await test_snapshot(client)


def main() -> int:
    global HTTP_BASE
    parser = argparse.ArgumentParser(description="SGRN types simulation full-coverage test")
    parser.add_argument("--opcua", default="opc.tcp://localhost:4840", metavar="URL")
    parser.add_argument("--http",  default="http://localhost:8000",    metavar="URL")
    args = parser.parse_args()
    HTTP_BASE = args.http

    asyncio.run(run(args.opcua))

    passed     = sum(1 for _, ok, _ in RESULTS if ok)
    failed     = sum(1 for _, ok, _ in RESULTS if not ok)
    total      = len(RESULTS)
    enum_fails = [(n, d) for n, ok, d in RESULTS if not ok and n.startswith("[ENUM]")]
    hard_fails = [(n, d) for n, ok, d in RESULTS if not ok and not n.startswith("[ENUM]")]

    print(f"\n{'='*66}")
    print(f"Results: {passed}/{total} passed  |  {failed} failed")

    if enum_fails:
        print(f"\n  [WARN] Known enum issues ({len(enum_fails)} — tracked, non-fatal):")
        for name, detail in enum_fails:
            n = name.replace("[ENUM]", "")
            print(f"     {n}: {detail}")

    if hard_fails:
        print(f"\n  [FAIL] UNEXPECTED FAILURES ({len(hard_fails)}):")
        for name, detail in hard_fails:
            print(f"     {name}: {detail}")
        return 1

    print("\n[PASS] All non-enum tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
