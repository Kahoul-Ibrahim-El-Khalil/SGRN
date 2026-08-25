#!/usr/bin/env python3
"""
test_enum_edgecases.py — Exercises every enum read/write/registration path
against the `enums_advanced` simulation (see schema.scl in this directory).

Requires: asyncua (`pip install asyncua`) and requests (`pip install requests`)
for the HTTP ground-truth cross-check.

Usage:
    python3 test_enum_edgecases.py --opcua opc.tcp://localhost:4840 --http http://localhost:8000

This is a correctness harness, not a load test — it runs sequentially and
prints a PASS/FAIL line per case, then a summary at the end with a non-zero
exit code if anything failed. It's meant to be run repeatedly across fresh
gateway restarts (see the identity-token fix's step 2.2) as well as against
a long-running gateway (to catch the enum bugs specifically).
"""

import argparse
import asyncio
import sys

import requests
from asyncua import Client, ua

RESULTS = []


def record(name: str, ok: bool, detail: str = ""):
    RESULTS.append((name, ok, detail))
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {name}" + (f" — {detail}" if detail and not ok else ""))


async def read_back_http(http_base: str, path: str):
    """Cross-check via HTTP so a passing OPC-UA read alone can't hide a
    write that silently went to the wrong place."""
    resp = requests.get(f"{http_base}/api/plc/DB1/{path}")
    resp.raise_for_status()
    return resp.json().get("value")


async def find_node(client: Client, browse_path):
    """Resolve a node under Objects/DB1 by a list of browse names."""
    objects = client.nodes.objects
    node = objects
    for name in browse_path:
        node = await node.get_child(f"1:{name}")
    return node


async def write_and_verify(client, http_base, browse_path, http_path, value, ua_type, expect_ok=True, expect_value=None):
    """Write `value` (already boxed as the right ua.Variant) to a node, then
    verify via both an OPC-UA read-back and an independent HTTP read."""
    name = f"write {'/'.join(browse_path)} = {value!r}"
    try:
        node = await find_node(client, browse_path)
        await node.write_value(ua.Variant(value, ua_type))
    except ua.UaStatusCodeError as e:
        if expect_ok:
            record(name, False, f"unexpected write failure: {e}")
        else:
            record(name, True, f"correctly rejected: {e}")
        return
    if not expect_ok:
        record(name, False, "write succeeded but was expected to be rejected")
        return

    try:
        readback = await node.read_value()
    except Exception as e:
        record(name, False, f"read-back failed: {e}")
        return

    want = expect_value if expect_value is not None else value
    ok = readback == want
    if ok and http_base and http_path:
        try:
            http_val = await read_back_http(http_base, http_path)
            ok = ok and (http_val == want)
            if not ok:
                record(name, False, f"OPC-UA read-back={readback} but HTTP read-back={http_val} (want {want})")
                return
        except Exception as e:
            record(name, False, f"HTTP cross-check failed: {e}")
            return
    record(name, ok, f"got {readback}, want {want}")


async def write_expect_rejected(client, browse_path, value, ua_type):
    name = f"reject write {'/'.join(browse_path)} = {value!r}"
    node = await find_node(client, browse_path)
    before = await node.read_value()
    try:
        await node.write_value(ua.Variant(value, ua_type))
        after = await node.read_value()
        if after == before:
            record(name, True, "write accepted by stack but value unchanged (also acceptable)")
        else:
            record(name, False, f"out-of-range write was NOT rejected — value changed to {after}")
    except ua.UaStatusCodeError as e:
        record(name, True, f"correctly rejected: {e}")


async def run(opcua_url: str, http_base: str):
    async with Client(url=opcua_url) as client:

        # ── 1. Baseline: #ENUM shorthand round-trip, every declared value ──
        for val in (0, 1, 2, 3):
            await write_and_verify(client, http_base, ["EnumEdgeCases", "status"], "status", val, ua.VariantType.Int32)

        # ── 2. Native TIA syntax, including the auto-incremented member ──
        # STOPPED=0 ... STOP=5, UKNOWN auto-increments to 6.
        for val in (0, 3, 5, 6):
            await write_and_verify(client, http_base, ["EnumEdgeCases", "plant_mode"], "plant_mode", val, ua.VariantType.Int32)
        # UKNOWN+1 (7) was never declared — must be rejected.
        await write_expect_rejected(client, ["EnumEdgeCases", "plant_mode"], 7, ua.VariantType.Int32)

        # ── 3. Negative values (SInt-backed) ──
        for val in (-1, 0, 1):
            await write_and_verify(client, http_base, ["EnumEdgeCases", "sign_bit"], "sign_bit", val, ua.VariantType.Int32)
        # -2 and 2 are in-range for SInt but not declared members.
        await write_expect_rejected(client, ["EnumEdgeCases", "sign_bit"], -2, ua.VariantType.Int32)
        await write_expect_rejected(client, ["EnumEdgeCases", "sign_bit"], 2, ua.VariantType.Int32)

        # ── 4. Full 32-bit boundary values (DInt-backed) ──
        for val in (-2000000000, 0, 2000000000):
            await write_and_verify(client, http_base, ["EnumEdgeCases", "wide_enum"], "wide_enum", val, ua.VariantType.Int32)
        # One past INT32 range on the low side should fail cleanly, not
        # wrap/crash — this stresses isInRange<int32_t> at the true boundary.
        await write_expect_rejected(client, ["EnumEdgeCases", "wide_enum"], -2147483648, ua.VariantType.Int32)

        # ── 5. Unsigned byte boundary — 255 must not be misread as negative
        for val in (0, 127, 255):
            await write_and_verify(client, http_base, ["EnumEdgeCases", "byte_max"], "byte_max", val, ua.VariantType.Int32)
        await write_expect_rejected(client, ["EnumEdgeCases", "byte_max"], 256, ua.VariantType.Int32)

        # ── 6. Sparse set — 500 is in-range for UInt but not a declared
        #      member; membership must be checked, not just width/range.
        for val in (10, 20, 21, 1000):
            await write_and_verify(client, http_base, ["EnumEdgeCases", "sparse_gap"], "sparse_gap", val, ua.VariantType.Int32)
        await write_expect_rejected(client, ["EnumEdgeCases", "sparse_gap"], 500, ua.VariantType.Int32)
        await write_expect_rejected(client, ["EnumEdgeCases", "sparse_gap"], 11, ua.VariantType.Int32)

        # ── 7. Duplicate-signature dedup: "Status" and "StatusAlias" have
        #      identical base type + value set but are different SCL types.
        #      Both must read/write independently and correctly even if the
        #      registry collapses them to one UA_DataType internally.
        await write_and_verify(client, http_base, ["EnumEdgeCases", "status"], "status", 1, ua.VariantType.Int32)
        await write_and_verify(client, http_base, ["EnumEdgeCases", "status_alias"], "status_alias", 3, ua.VariantType.Int32)
        s1 = await (await find_node(client, ["EnumEdgeCases", "status"])).read_value()
        s2 = await (await find_node(client, ["EnumEdgeCases", "status_alias"])).read_value()
        record("dedup independence: status != status_alias after distinct writes", s1 == 1 and s2 == 3, f"status={s1} status_alias={s2}")

        # ── 8. Inline field-level #ENUM (no named UDT alias at all) ──
        for val in (0, 1, 2):
            await write_and_verify(
                client, http_base, ["EnumEdgeCases", "inline_priority"], "inline_priority", val, ua.VariantType.Int32
            )

        # ── 9. Enum inside a struct-array element ──
        for idx, val in enumerate((0, 1, 2, 3)):
            await write_and_verify(
                client, http_base, ["EnumEdgeCases", "alarms", str(idx), "priority"],
                f"alarms[{idx}].priority", val, ua.VariantType.Int32,
            )

        # ── 10. Array of a bare enum-typed scalar alias ──
        for idx in range(10):
            val = idx % 7  # cycles through all PlantModeNative values incl. auto-incremented UKNOWN=6
            await write_and_verify(
                client, http_base, ["EnumEdgeCases", "mode_log", str(idx)], f"mode_log[{idx}]", val, ua.VariantType.Int32
            )

        # ── 11. Nested UDT-of-UDT, enum two levels deep ──
        await write_and_verify(
            client, http_base, ["EnumEdgeCases", "station", "mode"], "station.mode", 2, ua.VariantType.Int32
        )
        await write_and_verify(
            client, http_base, ["EnumEdgeCases", "station", "last_alarm", "priority"],
            "station.last_alarm.priority", 3, ua.VariantType.Int32,
        )

        # ── 12. Regression control: plain non-enum field unaffected ──
        await write_and_verify(client, http_base, ["EnumEdgeCases", "counter"], "counter", 424242, ua.VariantType.Int32)

        # ── 13. Type-mismatch: writing a String to an enum node must be
        #       rejected with BadTypeMismatch, not silently coerced.
        name = "reject String write to enum node"
        try:
            node = await find_node(client, ["EnumEdgeCases", "status"])
            await node.write_value(ua.Variant("RUNNING", ua.VariantType.String))
            record(name, False, "String write to an enum node was accepted")
        except ua.UaStatusCodeError as e:
            record(name, True, f"correctly rejected: {e}")

        # ── 14. Browse sanity: the enum DataTypes themselves must be
        #       browsable with correct, non-garbage DisplayNames — this is
        #       the direct regression check for the dangling-typeName bug.
        try:
            status_node = await find_node(client, ["EnumEdgeCases", "status"])
            dtype_node = client.get_node(await status_node.read_data_type())
            display_name = (await dtype_node.read_display_name()).Text
            ok = bool(display_name) and display_name.isprintable() and "Status" in display_name
            record("enum DataType DisplayName is sane (not garbage)", ok, f"got {display_name!r}")
        except Exception as e:
            record("enum DataType DisplayName is sane (not garbage)", False, str(e))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--opcua", default="opc.tcp://localhost:4840")
    parser.add_argument("--http", default="http://localhost:8000")
    args = parser.parse_args()

    asyncio.run(run(args.opcua, args.http))

    passed = sum(1 for _, ok, _ in RESULTS if ok)
    failed = sum(1 for _, ok, _ in RESULTS if not ok)
    print(f"\n{passed} passed, {failed} failed, {len(RESULTS)} total")
    if failed:
        print("\nFailures:")
        for name, ok, detail in RESULTS:
            if not ok:
                print(f"  - {name}: {detail}")
        sys.exit(1)


if __name__ == "__main__":
    main()
