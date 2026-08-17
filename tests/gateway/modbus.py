#!/usr/bin/env python3
"""
test_modbus_support.py — Integration test for S7 Gateway Modbus TCP support.

Tests:
  1. Modbus TCP Write Registers (FC16): writes floats, ints, and padded USInts to DB17.
  2. Modbus TCP Read Registers (FC03): reads back DB17 to verify big-endian encoding and padding.
  3. HTTP GET: verifies values were synchronized correctly to the PlcMemory digital twin.
  4. Modbus TCP Write Coils (FC15): writes boolean bits to DB18.
  5. Modbus TCP Read Coils (FC01): reads back DB18 coils.
  6. HTTP GET: verifies coil boolean values in PlcMemory digital twin.

No external python dependencies are required for Modbus (uses standard library 'socket').
Requires 'requests' for verifying via the northbound HTTP API.
"""

import argparse
import json
import socket
import struct
import sys
import time

try:
    import requests
except ImportError:
    print("[FATAL] 'requests' package not found. Install with: pip install requests", file=sys.stderr)
    sys.exit(1)

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def ok(msg: str):
    print(f"  [PASS] {msg}")


def fail(msg: str):
    print(f"  [FAIL] {msg}", file=sys.stderr)
    sys.exit(1)


def send_modbus_request(ip: str, port: int, packet: bytes) -> bytes:
    """Send raw bytes over a TCP socket to the Modbus port and return the reply."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    try:
        s.connect((ip, port))
        s.sendall(packet)
        resp = s.recv(1024)
        return resp
    except Exception as e:
        fail(f"Modbus connection or socket error: {e}")
    finally:
        s.close()


# ─────────────────────────────────────────────────────────────────────────────
# Main Test Routine
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="SGRN Gateway — Modbus TCP Integration Test")
    parser.add_argument("--host", default="127.0.0.1", help="Gateway host (default: 127.0.0.1)")
    parser.add_argument("--http-port", type=int, default=8000, help="Gateway HTTP port (default: 8000)")
    parser.add_argument("--modbus-port", type=int, default=5020, help="Gateway Modbus TCP port (default: 5020)")
    args = parser.parse_args()

    http_base = f"http://{args.host}:{args.http_port}"
    mb_ip = args.host
    mb_port = args.modbus_port

    # Verify HTTP API is reachable
    try:
        requests.get(f"{http_base}/endpoints", timeout=3)
    except requests.exceptions.ConnectionError:
        print(f"[ERROR] Cannot connect to gateway at {http_base}. Is it running?", file=sys.stderr)
        sys.exit(1)

    print("=" * 70)
    print("  Passive Modbus TCP Slave Adapter Integration Test")
    print("=" * 70)

    # ─────────────────────────────────────────────────────────────────────────
    # Test 1: Write Holding Registers (FC16) to DB17
    # ─────────────────────────────────────────────────────────────────────────
    # DB17 structure:
    #   setpoint_temp : Real   (4 bytes -> 2 registers: address 0-1)
    #   command_speed : Int    (2 bytes -> 1 register : address 2)
    #   override_val  : USInt  (1 byte  -> 1 register : address 3, padded)
    #
    # Write values:
    #   setpoint_temp = 25.5  -> IEEE 754 hex 0x41cc0000 -> bytes: \x41\xcc\x00\x00
    #   command_speed = 1200  -> hex 0x04B0 -> bytes: \x04\xB0
    #   override_val  = 42    -> hex 0x2A -> bytes: \x2A\x00 (padded)
    #
    # Raw registers block to write: \x41\xcc\x00\x00\x04\xb0\x2a\x00
    # Total registers = 4
    # Total bytes = 8
    #
    # Modbus Write Request Packet (FC16):
    #   Transaction ID: \x00\x01
    #   Protocol ID:    \x00\x00
    #   Length:         \x00\x0F (15 bytes following)
    #   Unit ID:        \x01
    #   Function Code:  \x10 (FC16)
    #   Start Address:  \x00\x00
    #   Reg Count:      \x00\x04
    #   Byte Count:     \x08
    #   Data:           \x41\xcc\x00\x00\x04\xb0\x2a\x00
    print("\n[1] Writing Holding Registers (FC16) to DB17 (Address 0, length 4)...")
    write_regs_packet = (
        b"\x00\x01" +  # Transaction ID
        b"\x00\x00" +  # Protocol ID
        b"\x00\x0f" +  # Length
        b"\x01" +      # Unit ID
        b"\x10" +      # Function Code (FC16)
        b"\x00\x00" +  # Starting Address (0)
        b"\x00\x04" +  # Number of Registers (4)
        b"\x08" +      # Byte count (8)
        b"\x41\xcc\x00\x00\x04\xb0\x2a\x00"  # Data
    )
    resp = send_modbus_request(mb_ip, mb_port, write_regs_packet)
    # Expected response: Transaction(0001) Protocol(0000) Length(0006) Unit(01) FC(10) Start(0000) RegCount(0004)
    expected_resp = b"\x00\x01\x00\x00\x00\x06\x01\x10\x00\x00\x00\x04"
    if resp != expected_resp:
        fail(f"Write Registers response mismatch. Expected:\n  {expected_resp.hex()}\nGot:\n  {resp.hex()}")
    ok("Write Holding Registers (FC16) succeeded.")

    # Sleep briefly to let background thread sync
    time.sleep(0.2)

    # ─────────────────────────────────────────────────────────────────────────
    # Test 2: Verify registers via HTTP GET from digital twin
    # ─────────────────────────────────────────────────────────────────────────
    print("\n[2] Verifying registers in S7 digital twin via HTTP GET...")
    r = requests.get(f"{http_base}/data/ModbusDatablock", timeout=3)
    if r.status_code != 200:
        fail(f"HTTP GET /data/ModbusDatablock failed with {r.status_code}: {r.text}")
    db_data = r.json()
    print(f"  ModbusDatablock Twin state: {json.dumps(db_data)}")

    # Check setpoint_temp
    val_temp = db_data.get("setpoint_temp")
    if val_temp is None or abs(float(val_temp) - 25.5) > 1e-5:
        fail(f"Setpoint temperature mismatch. Expected 25.5, got {val_temp}")
    # Check command_speed
    val_speed = db_data.get("command_speed")
    if val_speed is None or int(val_speed) != 1200:
        fail(f"Command speed mismatch. Expected 1200, got {val_speed}")
    # Check override_val (tests depadding / odd byte logic)
    val_override = db_data.get("override_val")
    if val_override is None or int(val_override) != 42:
        fail(f"Override value mismatch (padding check). Expected 42, got {val_override}")
    ok("Digital twin synchronized and verified correctly.")

    # ─────────────────────────────────────────────────────────────────────────
    # Test 3: Read back Holding Registers (FC03)
    # ─────────────────────────────────────────────────────────────────────────
    print("\n[3] Reading back Holding Registers (FC03)...")
    read_regs_packet = (
        b"\x00\x02" +  # Transaction ID
        b"\x00\x00" +  # Protocol ID
        b"\x00\x06" +  # Length
        b"\x01" +      # Unit ID
        b"\x03" +      # Function Code (FC03)
        b"\x00\x00" +  # Starting Address (0)
        b"\x00\x04"    # Number of Registers (4)
    )
    resp = send_modbus_request(mb_ip, mb_port, read_regs_packet)
    # Expected response: Transaction(0002) Protocol(0000) Length(000b) Unit(01) FC(03) ByteCount(08) Data(8 bytes)
    expected_header = b"\x00\x02\x00\x00\x00\x0b\x01\x03\x08"
    if not resp.startswith(expected_header):
        fail(f"Read Registers response header mismatch. Got:\n  {resp.hex()}")
    data_part = resp[9:]
    expected_data = b"\x41\xcc\x00\x00\x04\xb0\x2a\x00"
    if data_part != expected_data:
        fail(f"Read Registers data mismatch. Expected: {expected_data.hex()}, got: {data_part.hex()}")
    ok("Read Holding Registers (FC03) verification succeeded.")

    # ─────────────────────────────────────────────────────────────────────────
    # Test 4: Write Coils (FC15) to DB18
    # ─────────────────────────────────────────────────────────────────────────
    # DB18 structure:
    #   run_command   : Bool  (Coil 0)
    #   reset_trip    : Bool  (Coil 1)
    #   bypass_active : Bool  (Coil 2)
    #
    # Write values:
    #   run_command = True
    #   reset_trip = False
    #   bypass_active = True
    #
    # Bitmask: bit 0 (run_command) = 1, bit 1 (reset_trip) = 0, bit 2 (bypass_active) = 1 -> binary 101 = 5 (0x05)
    # Total coils = 3
    #
    # Modbus Write Request Packet (FC15):
    #   Transaction ID: \x00\x03
    #   Protocol ID:    \x00\x00
    #   Length:         \x00\x08 (8 bytes following)
    #   Unit ID:        \x01
    #   Function Code:  \x0F (FC15)
    #   Start Address:  \x00\x00
    #   Coil Count:     \x00\x03
    #   Byte Count:     \x01
    #   Data:           \x05
    print("\n[4] Writing Coils (FC15) to DB18 (Address 0, length 3)...")
    write_coils_packet = (
        b"\x00\x03" +  # Transaction ID
        b"\x00\x00" +  # Protocol ID
        b"\x00\x08" +  # Length
        b"\x01" +      # Unit ID
        b"\x0f" +      # Function Code (FC15)
        b"\x00\x00" +  # Starting Address (0)
        b"\x00\x03" +  # Number of Coils (3)
        b"\x01" +      # Byte count (1)
        b"\x05"        # Data (binary 101)
    )
    resp = send_modbus_request(mb_ip, mb_port, write_coils_packet)
    # Expected response: Transaction(0003) Protocol(0000) Length(0006) Unit(01) FC(0F) Start(0000) CoilCount(0003)
    expected_resp = b"\x00\x03\x00\x00\x00\x06\x01\x0f\x00\x00\x00\x03"
    if resp != expected_resp:
        fail(f"Write Coils response mismatch. Expected:\n  {expected_resp.hex()}\nGot:\n  {resp.hex()}")
    ok("Write Coils (FC15) succeeded.")

    # Sleep briefly to let background thread sync
    time.sleep(0.2)

    # ─────────────────────────────────────────────────────────────────────────
    # Test 5: Verify coils via HTTP GET from digital twin
    # ─────────────────────────────────────────────────────────────────────────
    print("\n[5] Verifying coils in S7 digital twin via HTTP GET...")
    r = requests.get(f"{http_base}/data/ModbusCoils", timeout=3)
    if r.status_code != 200:
        fail(f"HTTP GET /data/ModbusCoils failed with {r.status_code}: {r.text}")
    db_data = r.json()
    print(f"  ModbusCoils Twin state: {json.dumps(db_data)}")

    # Check values
    if db_data.get("run_command") is not True:
        fail(f"run_command mismatch. Expected true, got {db_data.get('run_command')}")
    if db_data.get("reset_trip") is not False:
        fail(f"reset_trip mismatch. Expected false, got {db_data.get('reset_trip')}")
    if db_data.get("bypass_active") is not True:
        fail(f"bypass_active mismatch. Expected true, got {db_data.get('bypass_active')}")
    ok("Digital twin synchronized and verified correctly.")

    # ─────────────────────────────────────────────────────────────────────────
    # Test 6: Read back Coils (FC01)
    # ─────────────────────────────────────────────────────────────────────────
    print("\n[6] Reading back Coils (FC01)...")
    read_coils_packet = (
        b"\x00\x04" +  # Transaction ID
        b"\x00\x00" +  # Protocol ID
        b"\x00\x06" +  # Length
        b"\x01" +      # Unit ID
        b"\x01" +      # Function Code (FC01)
        b"\x00\x00" +  # Starting Address (0)
        b"\x00\x03"    # Number of Coils (3)
    )
    resp = send_modbus_request(mb_ip, mb_port, read_coils_packet)
    # Expected response: Transaction(0004) Protocol(0000) Length(0004) Unit(01) FC(01) ByteCount(01) Data(05)
    expected_resp = b"\x00\x04\x00\x00\x00\x04\x01\x01\x01\x05"
    if resp != expected_resp:
        fail(f"Read Coils response mismatch. Expected:\n  {expected_resp.hex()}\nGot:\n  {resp.hex()}")
    ok("Read Coils (FC01) verification succeeded.")

    print("\n" + "=" * 70)
    print("  All Modbus TCP Slave Integration Tests PASSED!")
    print("=" * 70 + "\n")


if __name__ == "__main__":
    main()
