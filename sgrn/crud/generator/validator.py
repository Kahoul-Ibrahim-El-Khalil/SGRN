from typing import Dict, Any
from schema_provider import PostgresSchemaProvider

class ManifestValidator:
    def __init__(self, schema_provider: PostgresSchemaProvider):
        self.schema_provider = schema_provider

    def validate(self, manifest: Dict[str, Any]) -> bool:
        if "table" not in manifest or "schema" not in manifest["table"] or "name" not in manifest["table"]:
            print(f"[VALIDATION ERROR] Manifest missing table.schema or table.name")
            return False

        schema_name = manifest["table"]["schema"]
        table_name = manifest["table"]["name"]

        if not self.schema_provider.table_exists(schema_name, table_name):
            print(f"[VALIDATION ERROR] Table '{schema_name}.{table_name}' does not exist in database")
            return False

        if "tenant" not in manifest or "column" not in manifest["tenant"]:
            print(f"[VALIDATION ERROR] Manifest for '{table_name}' missing tenant.column definition")
            return False

        if "fields" not in manifest or not manifest["fields"]:
            print(f"[VALIDATION ERROR] Manifest for '{table_name}' contains no fields definition")
            return False

        return True
