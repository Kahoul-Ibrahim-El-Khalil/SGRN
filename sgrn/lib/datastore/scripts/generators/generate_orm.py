#!/usr/bin/env python3
import os
import json
import argparse
import subprocess
import shutil

# Defaults
DEFAULT_TARGET_DIR = "src/orm/models"
DEFAULT_SCHEMAS = ["core", "storage"]

# Database Defaults
DB_CONFIG_TEMPLATE = {
    "rdbms": "postgresql",
    "host": "127.0.0.1",
    "port": 5432,
    "dbname": "sgrn",
    "user": "sgrn_datastore",
    "passwd": "dracaeris",
    "tables": [],  # Empty list = Auto-discover all tables
    "schema": ""   # Will be replaced per loop
}

def generate_model_orm(schema, target_dir, db_config):
    print(f">> Generating standard tables for schema: {schema}")
    
    orm_schema_dir = os.path.join(target_dir, schema)
    
    # 1. Create directory (mkdir -p)
    os.makedirs(orm_schema_dir, exist_ok=True)
    
    # 2. Prepare config for this specific schema
    current_config = db_config.copy()
    current_config["schema"] = schema
    
    config_path = os.path.join(orm_schema_dir, "model.json")
    
    # 3. Write JSON config
    # 'with' block ensures file is flushed and closed BEFORE we run the command
    with open(config_path, 'w') as f:
        json.dump(current_config, f, indent=4)
    
    # 4. Run drogon_ctl
    # We use subprocess.run for better safety than os.system
    try:
        cmd = ["drogon_ctl", "create", "model", orm_schema_dir]
        subprocess.run(cmd, check=True, input=b"y\n")
    except subprocess.CalledProcessError as e:
        print(f"Error generating model for {schema}: {e}")

def main():
    # Parse command line flags
    parser = argparse.ArgumentParser(description="Generate Drogon ORM Models")
    parser.add_argument("--clean", action="store_true", help="Clean the target directory before generating")
    parser.add_argument("--password", type=str, default=DB_CONFIG_TEMPLATE["passwd"], help="Database password")
    parser.add_argument("--host", type=str, default=DB_CONFIG_TEMPLATE["host"], help="Database host")
    parser.add_argument("--port", type=int, default=DB_CONFIG_TEMPLATE["port"], help="Database port")
    args = parser.parse_args()

    target_dir = DEFAULT_TARGET_DIR
    
    # Update password if flag provided
    db_config = DB_CONFIG_TEMPLATE.copy()
    if args.password:
        db_config["passwd"] = args.password
    if args.host:
        db_config["host"] = args.host
    if args.port:
        db_config["port"] = args.port

    # Optional: Clean old models
    if args.clean and os.path.exists(target_dir):
        print(f"Cleaning {target_dir}...")
        shutil.rmtree(target_dir)

    # Ensure base directory exists
    os.makedirs(target_dir, exist_ok=True)

    # Generate for all schemas
    for schema in DEFAULT_SCHEMAS:
        generate_model_orm(schema, target_dir, db_config)

    print("Model Generation done")

if __name__ == "__main__":
    main()
