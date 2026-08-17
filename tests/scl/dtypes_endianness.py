#!/usr/bin/env python3
"""
Regression test for schema-aware per-field endianness in ``sgrn.dtypes``.

Verifies that mixed-endian DB fields decode correctly through a NumPy
structured dtype, including a nested UDT whose fields inherit the parent
instance's endianness unless they override it themselves.
"""

import os
import struct
import sys

import numpy as np

# The Python bindings live under sgrn/bindings/python; put that on the path
# BEFORE the repo root so the C++ sgrn/ tree doesn't shadow the bindings pkg.
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _REPO_ROOT)
sys.path.insert(0, os.path.join(_REPO_ROOT, "sgrn", "bindings", "python"))

from sgrn.dtypes import decode_record
from sgrn.models import DbField, DbSchema, UdtSchema


def main() -> None:
    mixed_udt = UdtSchema(
        udt_number=1,
        name="MixedBlock",
        size_bytes=4,
        fields=[
            DbField(
                name="nested_be_word",
                offset=0,
                bit_index=0,
                type="WORD",
                endianness="big",
            ),
            DbField(
                name="nested_inherit_int",
                offset=2,
                bit_index=0,
                type="INT",
            ),
        ],
    )

    schema = DbSchema(
        db_number=99,
        db_name="MixedEndian",
        size_bytes=12,
        fields=[
            DbField(
                name="root_be_real",
                offset=0,
                bit_index=0,
                type="REAL",
                endianness="big",
            ),
            DbField(
                name="mixed_block",
                offset=4,
                bit_index=0,
                type="STRUCT",
                udt_name="MixedBlock",
                struct_size=4,
                endianness="little",
            ),
            DbField(
                name="tail_le_udint",
                offset=8,
                bit_index=0,
                type="UDINT",
                endianness="little",
            ),
        ],
    )

    dt = schema.to_dtype(t_udts={mixed_udt.name: mixed_udt})

    raw = (
        struct.pack(">f", 42.5)
        + bytes((0xAB, 0xCD, 0x34, 0x12))
        + struct.pack("<I", 0x01020304)
    )

    record = np.frombuffer(raw, dtype=dt)[0]

    assert np.isclose(float(record["root_be_real"]), 42.5)
    assert int(record["tail_le_udint"]) == 0x01020304

    nested = record["mixed_block"]
    assert int(nested["nested_be_word"]) == 0xABCD
    assert int(nested["nested_inherit_int"]) == 0x1234

    decoded = decode_record(record, schema.fields, t_udts={mixed_udt.name: mixed_udt})
    assert np.isclose(decoded["root_be_real"], 42.5)
    assert decoded["tail_le_udint"] == 0x01020304
    assert decoded["mixed_block"]["nested_be_word"] == 0xABCD
    assert decoded["mixed_block"]["nested_inherit_int"] == 0x1234


if __name__ == "__main__":
    main()
