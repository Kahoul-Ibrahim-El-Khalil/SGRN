"""
sgrn.semantic — Semantic Interoperability Layer for SGRN Digital Twin.

Extracts rich semantic metadata (#RANGE, #ENUM, #READ_ONLY, units, descriptions, UDT structures)
from SCL schemas and projects raw memory state into typed, human-and-LLM-readable semantic representations.
"""

import json
import os
from dataclasses import dataclass, field
from typing import Dict, Any, List, Optional, Tuple, Union


@dataclass
class FieldMetadata:
    name: str
    db_name: str
    db_number: int
    data_type: str
    offset: int
    bit_index: int = 0
    count: int = 1
    unit: Optional[str] = None
    min_value: Optional[float] = None
    max_value: Optional[float] = None
    enum_map: Dict[str, int] = field(default_factory=dict)
    inv_enum_map: Dict[int, str] = field(default_factory=dict)
    read_only: bool = False
    description: Optional[str] = None

    def validate_value(self, val: Any) -> Tuple[bool, Optional[str]]:
        """Validate a proposed value against signal-level range/enum/read-only constraints (T1)."""
        if self.read_only:
            return False, f"Field '{self.db_name}.{self.name}' is READ_ONLY"

        if isinstance(val, (int, float)):
            if self.min_value is not None and val < self.min_value:
                return False, f"Value {val} below minimum range boundary {self.min_value} for '{self.name}'"
            if self.max_value is not None and val > self.max_value:
                return False, f"Value {val} exceeds maximum range boundary {self.max_value} for '{self.name}'"

        if self.enum_map:
            if isinstance(val, str):
                if val not in self.enum_map:
                    allowed = list(self.enum_map.keys())
                    return False, f"Enum string '{val}' not in allowed set {allowed} for '{self.name}'"
            elif isinstance(val, int):
                if val not in self.inv_enum_map:
                    allowed_indices = list(self.inv_enum_map.keys())
                    return False, f"Enum index {val} not in allowed set {allowed_indices} for '{self.name}'"

        return True, None


class SemanticSchema:
    """Loads and provides queryable semantic metadata for an SCL schema."""

    def __init__(self, schema_path_or_dict: Union[str, Dict[str, Any]]):
        self.raw_schema: Dict[str, Any] = {}
        self.fields: Dict[str, FieldMetadata] = {}  # key: "DbName.field_path"
        self.dbs: Dict[int, str] = {}  # db_num -> db_name
        self.udts: Dict[str, Any] = {}

        if isinstance(schema_path_or_dict, str):
            self._load_from_path(schema_path_or_dict)
        elif isinstance(schema_path_or_dict, dict):
            self.raw_schema = schema_path_or_dict
            self._parse_schema()

    def _load_from_path(self, path: str) -> None:
        if not os.path.exists(path):
            raise FileNotFoundError(f"Schema file not found: {path}")

        with open(path, "r", encoding="utf-8") as f:
            if path.endswith(".json"):
                self.raw_schema = json.load(f)
                self._parse_schema()
            else:
                self._parse_scl_text(f.read())

    def _parse_schema(self) -> None:
        dbs_node = self.raw_schema.get("data_blocks", self.raw_schema.get("dbs", []))
        for db in dbs_node:
            db_num = db.get("number", db.get("db_number", 1))
            db_name = db.get("name", db.get("db_name", f"DB{db_num}"))
            self.dbs[db_num] = db_name

            fields_node = db.get("fields", [])
            self._extract_fields(db_name, db_num, fields_node, prefix="")

    def _extract_fields(self, db_name: str, db_num: int, fields: List[Dict[str, Any]], prefix: str) -> None:
        for f in fields:
            name = f.get("name", "")
            full_path = f"{prefix}.{name}" if prefix else name
            key = f"{db_name}.{full_path}"

            enum_map = f.get("enum_map", {})
            inv_enum = {v: k for k, v in enum_map.items()} if enum_map else {}

            meta = FieldMetadata(
                name=full_path,
                db_name=db_name,
                db_number=db_num,
                data_type=f.get("type", "Real"),
                offset=f.get("offset", 0),
                bit_index=f.get("bit_index", 0),
                count=f.get("count", 1),
                unit=f.get("unit"),
                min_value=f.get("min_val", f.get("min_value")),
                max_value=f.get("max_val", f.get("max_value")),
                enum_map=enum_map,
                inv_enum_map=inv_enum,
                read_only=f.get("read_only", False),
                description=f.get("description"),
            )
            self.fields[key] = meta

            if "children" in f and f["children"]:
                self._extract_fields(db_name, db_num, f["children"], prefix=full_path)

    def _parse_scl_text(self, text: str) -> None:
        """Fallback parser for bare SCL declarations."""
        current_db = "Supervisor"
        db_num = 1
        for line in text.splitlines():
            line = line.strip()
            if line.startswith("DATA_BLOCK"):
                parts = line.split('"')
                if len(parts) >= 2:
                    current_db = parts[1]
                    self.dbs[db_num] = current_db
                    db_num += 1

    def get_field(self, db_name: str, field_path: str) -> Optional[FieldMetadata]:
        return self.fields.get(f"{db_name}.{field_path}")

    def format_semantic_summary(self, current_values: Dict[str, Any]) -> str:
        """Formats live state into a rich, semantic prompt string for LLM doer consumption."""
        lines = []
        for key, val in current_values.items():
            meta = self.fields.get(key)
            unit_str = f" {meta.unit}" if meta and meta.unit else ""
            desc_str = f" ({meta.description})" if meta and meta.description else ""

            enum_str = ""
            if meta and meta.inv_enum_map and isinstance(val, int) and val in meta.inv_enum_map:
                enum_str = f" [{meta.inv_enum_map[val]}]"

            lines.append(f" - {key}: {val}{enum_str}{desc_str}")
        return "\n".join(lines)
