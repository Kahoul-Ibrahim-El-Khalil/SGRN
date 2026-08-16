#!/usr/bin/env python3
"""
Offline integration tests for the SGRN Python bindings.

Runs a local ``http.server`` that emulates the gateway REST surface and
asserts the Gateway client parses every endpoint into the typed models,
including the NumPy-backed raw-memory and schema-to-dtype paths.

Run: ``python3 tests/test_gateway_rest.py``
"""

import json
import os
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from sgrn import Gateway, GatewayError
from sgrn.models import (
    DataWriteResult,
    MemoryBatchWriteItem,
    MemoryReadItem,
    MemorySpan,
    RegistryResponse,
    SecurityPolicyResponse,
    to_base64url,
)

REGISTRY = {
    "dbs": [
        {
            "db_number": 10,
            "db_name": "ReactorCore",
            "size_bytes": 14,
            "endianness": "big",
            "fields": [
                {"name": "thermal_power_mw", "offset": 0, "bit_index": 0, "type": "REAL", "count": 1},
                {"name": "rods", "offset": 4, "bit_index": 0, "type": "INT", "count": 5},
            ],
        }
    ],
    "udts": [{"udt_number": 1, "name": "Temps", "size_bytes": 8, "fields": []}],
    "tags": [{"name": "RC_POWER", "table": "MOTORS", "address": "DB10.DBD0", "type": "REAL"}],
    "summary": {"total_dbs": 1, "total_udts": 1, "total_tags": 1, "accessible_dbs": 1, "warnings": 0},
}

CONNECTIONS = [
    {"type": "s7", "ip": "127.0.0.1", "endpoint": "102", "first_seen": 1, "last_seen": 2, "event_count": 3}
]

SESSIONS = [{"id": 1, "ip": "127.0.0.1", "connect_time": 1, "bytes_sent": 2, "bytes_received": 3}]
LOGS = [{"ts": 123, "level": "INFO", "msg": "booted"}]

POLICY = {
    "rules": [
        {
            "protocol": "HTTP",
            "action": "ALLOW",
            "specificity": 1,
            "cidrs": [],
            "dbs": [10],
            "any_db": False,
            "origins": [],
            "headers": ["X-SGRN-Key"],
            "sessions": [],
        }
    ],
    "total": 1,
    "mode": "strict",
}

ENDPOINTS = {"endpoints": [{"path": "/registry", "method": "GET", "description": "schema"}]}
BINARY_PAYLOAD = b"\x01\x02\x03\x04"

# Raw bytes for the whole ReactorCore DB: REAL(42.5) + INT[5](1..5), big-endian.
REACTOR_CORE_BYTES = struct.pack(">f", 42.5) + struct.pack(">5h", 1, 2, 3, 4, 5)

# Defined before the handler classes reference it, unlike the previous
# revision (which referenced this name from ``do_PUT`` before it was ever
# assigned at module scope — a ``NameError`` waiting to happen the first
# time ``PUT /memory/batch`` was actually exercised).
BATCH_WRITE_RESPONSE = [{"db": 1, "offset": 0, "size": 4, "written": to_base64url(BINARY_PAYLOAD)}]


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):  # silence
        pass

    def _write(self, data, code=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        q = parse_qs(parsed.query)
        path = parsed.path
        if path == "/registry":
            if q.get("headers") == ["true"]:
                payload = dict(REGISTRY)
                payload["dbs"] = [{k: v for k, v in d.items() if k != "fields"} for d in REGISTRY["dbs"]]
                return self._write(payload)
            if q.get("db"):
                payload = dict(REGISTRY)
                payload["udts"] = []
                payload["tags"] = []
                return self._write(payload)
            return self._write(REGISTRY)
        if path == "/registry/types":
            return self._write({"INT": 2})
        if path == "/registry/modbus":
            return self._write({"error": "Modbus support not initialized"}, 404)
        if path == "/data/DoesNotExist":
            return self._write({"error": "not found"}, 404)
        if path in ("/data", "/data/") or path.startswith("/data"):
            return self._write({"ReactorCore": {"thermal_power_mw": 42.5}})
        if path == "/memory/db/1/offset/0/size/4":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(BINARY_PAYLOAD)))
            self.end_headers()
            self.wfile.write(BINARY_PAYLOAD)
            return
        if path == "/memory/db/10/offset/0/size/14":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(REACTOR_CORE_BYTES)))
            self.end_headers()
            self.wfile.write(REACTOR_CORE_BYTES)
            return
        if path == "/memory/batch":
            return self._write([{"db": 1, "offset": 0, "size": 4, "data": to_base64url(BINARY_PAYLOAD)}])
        if path == "/connections":
            return self._write(CONNECTIONS)
        if path == "/db/history":
            return self._write({"series": []})
        if path == "/db/sessions":
            return self._write(SESSIONS)
        if path == "/db/logs":
            return self._write(LOGS)
        if path == "/endpoints":
            return self._write(ENDPOINTS)
        if path == "/api/policy":
            return self._write(POLICY)
        return self._write({"error": "not found"}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        if urlparse(self.path).path in ("/data", "/data/"):
            return self._write({"fields_written": 2})
        if urlparse(self.path).path.startswith("/data/"):
            try:
                value = json.loads(body)
            except Exception:
                return self._write({"error": "invalid"}, 400)
            return self._write({"db": 10, "path": self.path, "fields_written": 1, "value": value})
        return self._write({"error": "not found"}, 404)

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        path = urlparse(self.path).path
        if path == "/memory/batch":
            return self._write(BATCH_WRITE_RESPONSE)
        if path.startswith("/data"):
            ctype = self.headers.get("Content-Type", "")
            if "json" in ctype:
                try:
                    value = json.loads(body)
                except Exception:
                    value = body.decode("utf-8", "replace")
                return self._write({"db": 1, "path": self.path, "fields_written": 3, "value": value})
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path.startswith("/memory/"):
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        return self._write({"error": "not found"}, 404)


def main() -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()

    gw = Gateway(f"http://127.0.0.1:{port}")

    # Registry
    reg = gw.registry()
    assert isinstance(reg, RegistryResponse)
    assert len(reg.dbs) == 1 and reg.dbs[0].db_name == "ReactorCore"
    assert reg.dbs[0].fields[0].type == "REAL"
    assert reg.db_by_number(10) is reg.dbs[0]
    assert reg.udts[0].name == "Temps"
    assert reg.tags[0].address == "DB10.DBD0"
    assert gw.registry(t_headers_only=True).dbs[0].fields == []
    assert gw.registry(t_db=10).dbs[0].db_number == 10

    # Registry aux
    assert gw.registry_types() == {"INT": 2}
    try:
        gw.modbus_map()
        raise AssertionError("modbus_map should 404")
    except GatewayError as e:
        assert getattr(e, "status", None) == 404

    # Data
    assert gw.read_data()["ReactorCore"]["thermal_power_mw"] == 42.5
    assert gw.read_data("ReactorCore/speed") is not None
    res = gw.write_fields("ReactorCore", {"speed": 1.0})
    assert isinstance(res, DataWriteResult) and res.fields_written == 1
    assert gw.write_field("ReactorCore/speed", 1.0).value == 1.0
    assert gw.write_array_element("DB2/temperatures", 2, 42.0).fields_written == 1
    assert gw.write_multi_db({"ReactorCore": {"speed": 2.0}}) == {"fields_written": 2}
    assert gw.replace_field("ReactorCore/speed", 3.0).fields_written > 0

    # Memory (raw bytes)
    assert gw.memory_read(1, 0, 4) == BINARY_PAYLOAD
    assert gw.memory_write(1, 0, BINARY_PAYLOAD) == BINARY_PAYLOAD
    reads = gw.memory_batch_read([MemorySpan(1, 0, 4)])
    assert len(reads) == 1 and isinstance(reads[0], MemoryReadItem) and reads[0].data == BINARY_PAYLOAD
    batch = gw.memory_batch_write([MemoryBatchWriteItem(1, 0, BINARY_PAYLOAD)])
    assert batch[0].written == BINARY_PAYLOAD

    # Memory (NumPy) — dtype param on memory_read/memory_batch_read,
    # ndarray input on memory_write/memory_batch_write.
    arr = gw.memory_read(1, 0, 4, t_dtype=">u1")
    assert isinstance(arr, np.ndarray) and arr.tolist() == [1, 2, 3, 4]
    np_payload = np.array([9, 9, 9, 9], dtype=">u1")
    assert gw.memory_write(1, 0, np_payload) == b"\x09\x09\x09\x09"
    np_batch = gw.memory_batch_write([MemoryBatchWriteItem(1, 0, np.array([1, 2, 3, 4], dtype=">u1"))])
    assert np_batch[0].written == BINARY_PAYLOAD
    arrays = gw.memory_batch_read([MemorySpan(1, 0, 4)], t_dtype=">u1")
    assert isinstance(arrays[0], np.ndarray) and arrays[0].tolist() == [1, 2, 3, 4]

    # Schema -> NumPy structured dtype, and a whole-DB structured read.
    dt = reg.dbs[0].to_dtype()
    assert dt.names == ("thermal_power_mw", "rods")
    assert dt.fields["thermal_power_mw"][0].itemsize == 4
    assert dt.fields["rods"][0].shape == (5,)
    record = gw.read_db_array("ReactorCore", t_registry=reg)
    assert abs(float(record["thermal_power_mw"]) - 42.5) < 1e-4
    assert list(record["rods"]) == [1, 2, 3, 4, 5]

    # Diagnostics
    conns = gw.connections()
    assert conns[0].ip == "127.0.0.1" and conns[0].type == "s7"
    assert gw.db_history() == {"series": []}
    assert gw.db_sessions()[0].bytes_sent == 2
    assert gw.db_logs(50)[0].level == "INFO"
    assert gw.endpoints()[0].path == "/registry"

    # Policy
    pol = gw.policy()
    assert isinstance(pol, SecurityPolicyResponse)
    assert pol.mode == "strict" and pol.rules[0].action == "ALLOW"

    # Errors
    try:
        gw.read_data("DoesNotExist")
        raise AssertionError("expected GatewayHTTPError on 404")
    except GatewayError as e:
        assert getattr(e, "status", None) == 404

    server.shutdown()
    server.server_close()
    print("ALL REST BINDINGS TESTS PASSED")


if __name__ == "__main__":
    main()
