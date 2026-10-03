import yaml
from pathlib import Path
from typing import List, Dict, Any

class ManifestLoader:
    def load_manifest(self, file_path: Path) -> Dict[str, Any]:
        with open(file_path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)

        self._normalize_manifest(data)
        return data

    def load_all(self, manifests_dir: Path) -> List[Dict[str, Any]]:
        manifests = []
        if not manifests_dir.exists():
            return manifests

        for file_path in sorted(manifests_dir.glob("*.yaml")):
            manifests.append(self.load_manifest(file_path))
        for file_path in sorted(manifests_dir.glob("*.yml")):
            manifests.append(self.load_manifest(file_path))

        return manifests

    def _normalize_manifest(self, data: Dict[str, Any]):
        # Ensure default structures
        if "policy" not in data:
            data["policy"] = {}

        policy = data["policy"]
        policy.setdefault("read_scope", "org")
        policy.setdefault("write_scope", "own")
        policy.setdefault("delete_scope", "admin")
        policy.setdefault("allowed_roles", ["admin", "user"])
        policy.setdefault("max_limit", 500)
        policy.setdefault("soft_delete", False)
        policy.setdefault("audit", True)
        policy.setdefault("versioning", False)
        policy.setdefault("batch_enabled", False)

        fields = data.get("fields", {})
        for name, field in fields.items():
            field_type = field.get("type", "text").lower()
            if field_type in ("int", "integer"):
                field["type_pascal"] = "Int"
            elif field_type in ("bigint", "long"):
                field["type_pascal"] = "BigInt"
            elif field_type in ("bool", "boolean"):
                field["type_pascal"] = "Bool"
            elif field_type in ("timestamp", "datetime", "timestamptz"):
                field["type_pascal"] = "Timestamp"
            elif field_type in ("jsonb", "json"):
                field["type_pascal"] = "Jsonb"
            else:
                field["type_pascal"] = "Text"

            # Operator bitmask strings
            ops = field.get("operators", [])
            op_flags = []
            for op in ops:
                op_lower = op.lower()
                if op_lower == "eq": op_flags.append("sgrn::crud::Op::Eq")
                elif op_lower == "neq": op_flags.append("sgrn::crud::Op::Neq")
                elif op_lower == "gt": op_flags.append("sgrn::crud::Op::Gt")
                elif op_lower == "gte": op_flags.append("sgrn::crud::Op::Gte")
                elif op_lower == "lt": op_flags.append("sgrn::crud::Op::Lt")
                elif op_lower == "lte": op_flags.append("sgrn::crud::Op::Lte")
                elif op_lower == "like": op_flags.append("sgrn::crud::Op::Like")
                elif op_lower == "in": op_flags.append("sgrn::crud::Op::In")

            if op_flags:
                field["op_flags_str"] = " | ".join(op_flags)
            else:
                field["op_flags_str"] = "sgrn::crud::Op::None"
