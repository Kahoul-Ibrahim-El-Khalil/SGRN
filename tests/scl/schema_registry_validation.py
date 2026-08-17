#!/usr/bin/env python3
import os
import sys
import subprocess
import json
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCLC_BIN = os.path.join(REPO_ROOT, ".prefix", "linux-static", "bin", "sclc")

SCL_SCHEMA = """
TYPE "MotorData"
VERSION : 0.1
   STRUCT
      speed : REAL;
      running : BOOL;
   END_STRUCT;
END_TYPE

DATA_BLOCK "Motors"
{ S7_Optimized_Access := 'FALSE' }
VERSION : 0.1
NON_RETAIN
   VAR
      pump_motor : "MotorData";
   END_VAR
BEGIN
END_DATA_BLOCK
"""

def main():
    if not os.path.exists(SCLC_BIN):
        print(f"SCL Compiler not found at {SCLC_BIN}")
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmpdir:
        input_file = os.path.join(tmpdir, "test.scl")
        output_file = os.path.join(tmpdir, "registry.json")

        with open(input_file, "w") as f:
            f.write(SCL_SCHEMA)

        print(f"Running sclc compile on test schema...")
        res = subprocess.run(
            [SCLC_BIN, "compile", "--file", input_file, "-o", output_file],
            capture_output=True,
            text=True
        )

        if res.returncode != 0:
            print(f"sclc compilation failed with exit code {res.returncode}")
            print(res.stderr)
            sys.exit(1)

        print(f"Compilation successful. Validating registry output...")
        
        with open(output_file, "r") as f:
            registry = json.load(f)

        # Basic structure validation
        assert "dbs" in registry, "Missing 'dbs' in registry"
        assert "udts" in registry, "Missing 'udts' in registry"
        
        # UDT Validation
        udts = registry["udts"]
        assert len(udts) == 1, f"Expected 1 UDT, found {len(udts)}"
        motor_data = udts[0]
        assert motor_data["name"] == "MotorData"
        assert len(motor_data["fields"]) == 2
        assert motor_data["fields"][0]["name"] == "speed"
        assert motor_data["fields"][0]["type"] == "REAL"
        assert motor_data["fields"][1]["name"] == "running"
        assert motor_data["fields"][1]["type"] == "BOOL"

        # DB Validation
        dbs = registry["dbs"]
        assert len(dbs) == 1, f"Expected 1 DB, found {len(dbs)}"
        motors_db = dbs[0]
        assert motors_db["db_name"] == "Motors"
        
        # SCL Compiler should resolve the fields
        fields = motors_db.get("fields", [])
        assert len(fields) == 1
        pump = fields[0]
        assert pump["name"] == "pump_motor"
        assert pump["type"] == "STRUCT"
        assert pump.get("udt_name") == "MotorData"

        print("Registry validation passed! SCL Compiler successfully processed the schema.")

if __name__ == "__main__":
    main()
