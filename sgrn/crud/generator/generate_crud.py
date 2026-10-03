#!/usr/bin/env python3
import os
import sys
from pathlib import Path

# Add current generator dir to Python path
sys.path.insert(0, str(Path(__file__).parent))

from schema_provider import PostgresSchemaProvider
from manifest_loader import ManifestLoader
from codegen import CppCodeGenerator, snake_to_pascal
from validator import ManifestValidator

def main():
    base_dir = Path(__file__).parent.parent
    manifests_dir = base_dir / "manifests"
    templates_dir = base_dir / "templates"
    output_dir = base_dir / "generated"

    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"[CRUD GENERATOR] Loading manifests from {manifests_dir}...")
    loader = ManifestLoader()
    manifests = loader.load_all(manifests_dir)

    if not manifests:
        print("[CRUD GENERATOR] No YAML manifests found!")
        return 0

    schema_provider = PostgresSchemaProvider(
        host=os.getenv("PGHOST", "localhost"),
        database=os.getenv("PGDATABASE", "sgrn_dev"),
        user=os.getenv("PGUSER", "postgres"),
        password=os.getenv("PGPASSWORD", "")
    )

    validator = ManifestValidator(schema_provider)
    codegen = CppCodeGenerator(templates_dir)

    handlers = []

    for manifest in manifests:
        table_name = manifest["table"]["name"]
        print(f"[CRUD GENERATOR] Validating manifest for '{table_name}'...")
        if not validator.validate(manifest):
            print(f"[CRUD GENERATOR] Validation failed for manifest '{table_name}'")
            return 1

        handler_code = codegen.generate_handler(manifest)
        output_file = output_dir / f"{table_name}.gen.hpp"
        output_file.write_text(handler_code, encoding="utf-8")

        class_name = f"{snake_to_pascal(table_name)}View"
        handlers.append({
            "table_name": table_name,
            "class_name": class_name,
            "file_basename": f"{table_name}.gen.hpp"
        })
        print(f"  ✓ Generated {output_file.relative_to(base_dir)}")

    registered_code = codegen.generate_registered_views(handlers)
    registered_file = output_dir / "RegisteredViews.cpp"
    registered_file.write_text(registered_code, encoding="utf-8")
    print(f"  ✓ Generated {registered_file.relative_to(base_dir)}")

    print(f"✓ Successfully generated {len(handlers)} CRUD view handlers and RegisteredViews.cpp")
    return 0

if __name__ == "__main__":
    sys.exit(main())
