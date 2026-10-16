import json
import os
import struct
import zlib
from typing import Dict, Any, List, Tuple, Optional

try:
    import zstandard as zstd
except ImportError:
    try:
        import zstd
    except ImportError:
        zstd = None

try:
    import numpy as np
except ImportError:
    np = None

try:
    import pandas as pd
except ImportError:
    pd = None

#: Reserved DB number marking a binary-WAL (.bin.zst) control frame.
#: Mirrors sgrn::gateway::database::kControlFrameDbNum in
#: sgrn/gateway/include/sgrn/gateway/database/PersistenceService.hpp.
#: Control frames (dictionary / manifest / anchor / footer) wrap JSON — not
#: raw DB memory — and must be skipped by frame decoders.
CONTROL_FRAME_DB_NUM = 0xFFFF

#: DB number marking a binary-WAL delta frame (v2+): payload is
#: db:u16 + (offset:u32, len:u32, bytes)* runs against the last image.
#: Mirrors sgrn::gateway::database::kDeltaFrameDbNum.
DELTA_FRAME_DB_NUM = 0xFFFE

#: DB number marking a binary-WAL anchor frame (v3+): payload is
#: db:u16 + crc32:u32 + full image. The CRC covers the image bytes.
#: Mirrors sgrn::gateway::database::kAnchorFrameDbNum.
ANCHOR_FRAME_DB_NUM = 0xFFFD

#: Newest binary WAL version this reader decodes (full + delta + anchor frames).
BINARY_WAL_VERSION = 3


def _findNextAnchor(raw: bytes, frm: int):
    """Byte-scan for the next CRC-verified anchor frame at/after frm.

    Returns the frame start offset, or None. The CRC makes false hits
    ~2^-32, so a hit is a trustworthy resync point after corruption.
    """
    n = len(raw)
    p = frm
    while p + 14 <= n:
        db_num, payload_len = struct.unpack("<HI", raw[p + 8:p + 14])
        if db_num == ANCHOR_FRAME_DB_NUM and payload_len >= 6 and p + 14 + payload_len <= n:
            pl = raw[p + 14:p + 14 + payload_len]
            (crc,) = struct.unpack("<I", pl[2:6])
            if zlib.crc32(pl[6:]) & 0xFFFFFFFF == crc:
                return p
        p += 1
    return None

class DatasetReader:
    """Reads sgrn_dataset manifests and normalized CSV telemetry records into NumPy/Pandas."""
    def __init__(self, manifest_path: str, csv_path: str):
        self.manifest_path = manifest_path
        self.csv_path = csv_path
        self.manifest: Dict[str, Any] = {}
        self.features: List[Dict[str, Any]] = []
        self._loadManifest()

    def _loadManifest(self) -> None:
        if not os.path.exists(self.manifest_path):
            raise FileNotFoundError(f"Manifest not found: {self.manifest_path}")
        with open(self.manifest_path, "r", encoding="utf-8") as f:
            self.manifest = json.load(f)
        self.features = self.manifest.get("features", [])

    def getFeatureNames(self, categorical_only: bool = False, continuous_only: bool = False) -> List[str]:
        names = []
        for feat in self.features:
            if categorical_only and not feat.get("is_categorical", False):
                continue
            if continuous_only and feat.get("is_categorical", False):
                continue
            names.append(feat["name"])
        return names

    def loadPandas(self) -> Optional["pd.DataFrame"]:
        if pd is None:
            raise ImportError("pandas library is required for loadPandas()")
        
        dtype_map = {}
        for feat in self.features:
            name = feat["name"]
            if feat.get("is_categorical", False):
                dtype_map[name] = "category"
            else:
                dtype_map[name] = "float32"

        df = pd.read_csv(self.csv_path, dtype=dtype_map)
        
        for feat in self.features:
            name = feat["name"]
            enum_vals = feat.get("enum_values")
            if enum_vals and name in df.columns:
                mapping = {int(k) if k.isdigit() else k: v for k, v in enum_vals.items()}
                df[name] = df[name].map(mapping).astype("category")

        df = df.ffill().bfill()
        num_cols = df.select_dtypes(include=["number", "float", "int"]).columns
        df[num_cols] = df[num_cols].fillna(0.0)
        return df

    def loadNumpy(self, target_column: Optional[str] = None) -> Tuple[Any, Optional[Any]]:
        if np is None:
            raise ImportError("numpy library is required for loadNumpy()")
        
        df = self.loadPandas()
        
        for col in df.select_dtypes(include=["category"]).columns:
            df[col] = df[col].cat.codes

        if target_column:
            if target_column not in df.columns:
                raise KeyError(f"Target column '{target_column}' not in dataset")
            feature_cols = [c for c in df.columns if c != target_column]
            X = df[feature_cols].to_numpy(dtype=np.float32)
            y = df[target_column].to_numpy(dtype=np.float32)
            X = np.nan_to_num(X, nan=0.0)
            y = np.nan_to_num(y, nan=0.0)
            return X, y
        
        X = df.to_numpy(dtype=np.float32)
        X = np.nan_to_num(X, nan=0.0)
        return X, None


class BinaryDatasetReader:
    """
    Directly reads and parses SGRN high-density Zstd binary WAL archives (.bin.zst).
    Extracts embedded SCL schema JSON header and raw C-struct memory frames into NumPy/Pandas.
    """
    def __init__(self, file_or_dir_path: str, scl_schema_path: Optional[str] = None):
        self.path = file_or_dir_path
        self.scl_schema_path = scl_schema_path
        self.files: List[str] = []
        self.schema_json: Dict[str, Any] = {}
        self.field_meta_list: List[Dict[str, Any]] = []

        self._discoverFiles()
        if self.files:
            self._loadHeaderSchema(self.files[0])

    def _discoverFiles(self) -> None:
        if os.path.isfile(self.path):
            if self.path.endswith('.bin.zst') or self.path.endswith('.zst') or self.path.endswith('.bin'):
                self.files.append(self.path)
        elif os.path.isdir(self.path):
            for root, _, files in os.walk(self.path):
                for f in files:
                    if f.endswith('.bin.zst') or f.endswith('.zst'):
                        self.files.append(os.path.join(root, f))
            self.files.sort()

        if not self.files:
            raise FileNotFoundError(f"No binary archive files found under path: {self.path}")

    def _decompressFile(self, file_path: str) -> bytes:
        # 1. Try system zstd CLI tool first if available (handles multi-frame streaming archives reliably)
        import subprocess
        try:
            res = subprocess.run(['zstd', '-d', '-c', file_path], capture_output=True, check=True)
            return res.stdout
        except Exception:
            pass

        # 2. Fallback to python zstandard library
        if zstd is not None:
            with open(file_path, 'rb') as f:
                compressed = f.read()
            if hasattr(zstd, 'ZstdDecompressor'):
                dctx = zstd.ZstdDecompressor()
                return dctx.decompress(compressed, max_output_size=100 * 1024 * 1024)
            elif hasattr(zstd, 'decompress'):
                try:
                    return zstd.decompress(compressed)
                except Exception:
                    dctx = zstd.ZstdDecompressor()
                    return dctx.decompress(compressed, max_output_size=100 * 1024 * 1024)
        
        raise RuntimeError(f"Failed to decompress {file_path}")

    def _loadHeaderSchema(self, file_path: str) -> None:
        raw = self._decompressFile(file_path)
        if len(raw) < 10 or raw[:4] != b'SGRN':
            raise ValueError(f"File {file_path} is not a valid SGRN binary archive (missing 'SGRN' magic header)")
        
        ver, schema_len = struct.unpack('<HI', raw[4:10])
        if schema_len > 0 and 10 + schema_len <= len(raw):
            schema_str = raw[10:10+schema_len].decode('utf-8', errors='ignore')
            try:
                self.schema_json = json.loads(schema_str)
                self._parseFieldMeta()
            except Exception:
                pass
        
        # If embedded header schema was empty and scl_schema_path was provided, load SCL schema file
        if not self.field_meta_list and self.scl_schema_path and os.path.exists(self.scl_schema_path):
            self._loadSclSchemaFile(self.scl_schema_path)

    def _loadSclSchemaFile(self, scl_path: str) -> None:
        import re
        with open(scl_path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()

        db_blocks = re.findall(r'DATA_BLOCK\s+"?([A-Za-z0-9_]+)"?.*?(?:STRUCT|BEGIN)(.*?)END_STRUCT', content, re.DOTALL | re.IGNORECASE)
        default_db_num = 1
        for db_name, struct_body in db_blocks:
            offset = 0
            lines = struct_body.strip().split(';')
            for line in lines:
                line = line.strip()
                if not line or ':' not in line:
                    continue
                var_part = line.split(':')[0].strip()
                var_name = var_part.split('#')[0].strip().split()[-1]
                type_part = line.split(':')[1].strip()
                type_str = type_part.split(':=')[0].strip().split('#')[0].strip()

                fmt_char = 'f'
                item_size = 4
                if type_str in ["REAL", "Float"]:
                    fmt_char = 'f'
                    item_size = 4
                elif type_str in ["LREAL", "Double"]:
                    fmt_char = 'd'
                    item_size = 8
                elif type_str in ["INT", "Int16"]:
                    fmt_char = 'h'
                    item_size = 2
                elif type_str in ["DINT", "Int32"]:
                    fmt_char = 'i'
                    item_size = 4
                elif type_str in ["BOOL", "Bool"]:
                    fmt_char = '?'
                    item_size = 1

                self.field_meta_list.append({
                    "full_name": f"{db_name}.{var_name}",
                    "db_num": default_db_num,
                    "db_name": db_name,
                    "field_name": var_name,
                    "offset": offset,
                    "type_str": type_str,
                    "fmt_char": fmt_char,
                    "item_size": item_size,
                    "kind": "float" if fmt_char in ("f", "d") else ("bool" if fmt_char == "?" else "float"),
                    "endian": ">",
                    "unit": ""
                })
                offset += item_size
            default_db_num += 1

    @staticmethod
    def _db_endianness(db_val: Dict[str, Any]) -> str:
        for key in ("endianness", "endian", "byte_order"):
            v = db_val.get(key)
            if isinstance(v, str) and v:
                return "<" if v.lower().startswith("l") else ">"
        return ""

    def _parseFieldMeta(self) -> None:
        self.field_meta_list.clear()
        dbs = self.schema_json.get("dbs", {})
        if isinstance(dbs, list):
            db_dict = {}
            for db in dbs:
                db_dict[db.get("db_number", 0)] = db
            dbs = db_dict
        raw_udts = self.schema_json.get("udts", self.schema_json.get("types", {}))
        udts = {}
        if isinstance(raw_udts, list):
            for u in raw_udts:
                if isinstance(u, dict) and u.get("name"):
                    udts[u["name"]] = u
        elif isinstance(raw_udts, dict):
            udts = raw_udts

        for db_key, db_val in dbs.items():
            db_num = int(db_key) if isinstance(db_key, (int, str)) and str(db_key).isdigit() else db_val.get("db_number", 0)
            db_name = db_val.get("db_name", f"DB{db_num}")
            db_endian = self._db_endianness(db_val)
            self._parseFieldList(db_name, db_num, db_endian, db_val.get("fields", []),
                                 udts, prefix="")

    @staticmethod
    def _scalarLayout(type_str: str, field: Dict[str, Any]) -> Tuple[Any, int, str]:
        """Map a scalar S7 type -> (struct format or None, byte size, kind)."""
        t = str(type_str).upper()
        if t in ("REAL", "SINGLE", "FLOAT"):
            return 'f', 4, "float"
        if t in ("LREAL", "DOUBLE"):
            return 'd', 8, "float"
        if t in ("INT", "INT16"):
            return 'h', 2, "int"
        if t in ("DINT", "INT32"):
            return 'i', 4, "int"
        if t == "SINT":
            return 'b', 1, "int"
        if t in ("USINT", "BYTE"):
            return 'B', 1, "int"
        if t in ("UINT", "WORD"):
            return 'H', 2, "int"
        if t in ("UDINT", "DWORD"):
            return 'I', 4, "int"
        if t == "LINT":
            return 'q', 8, "int"
        if t in ("ULINT", "LWORD"):
            return 'Q', 8, "int"
        if t in ("BOOL", "BOOL_"):
            return '?', 1, "bool"
        if t == "DTL":
            return None, 12, "dtl"
        if t in ("LDTL", "LDT"):
            return None, 8, "ldtl"
        if t in ("STRING", "XSTRING"):
            try:
                cap = int(field.get("capacity", 0) or 0)
            except (TypeError, ValueError):
                cap = 0
            return (None, 2 + cap, "string") if cap > 0 else (None, 2, "opaque")
        if t in ("WSTRING", "XWSTRING"):
            try:
                cap = int(field.get("capacity", 0) or 0)
            except (TypeError, ValueError):
                cap = 0
            return (None, 4 + cap * 2, "wstring") if cap > 0 else (None, 4, "opaque")
        return 'f', 4, "float"  # legacy default for unknown scalars

    def _parseFieldList(self, db_name: str, db_num: int, db_endian: str,
                        fields: List[Dict[str, Any]], udts: Dict[str, Any],
                        prefix: str, base_offset: Optional[int] = None) -> None:
        """Flatten registry fields to dotted meta entries.

        Inline ``children`` carry absolute DB offsets; UDT-referenced and
        array-expanded members are relative to their parent field offset
        (threaded through ``base_offset``). Array stride comes from
        ``struct_size``/UDT ``size_bytes`` when present, else the scalar
        element width.
        """
        for f in fields:
            if not isinstance(f, dict) or not f.get("name"):
                continue
            full_name = f"{db_name}.{prefix}{f['name']}" if prefix else f"{db_name}.{f['name']}"
            path = f"{prefix}{f['name']}" if prefix else f["name"]

            children = f.get("children") or []
            udt = udts.get(f.get("udt_name", "")) if f.get("udt_name") else None
            sub_fields = children or (udt.get("fields", []) if isinstance(udt, dict) else [])
            from_udt = (not children) and (udt is not None)
            if f.get("udt_name") and udt is None and not children:
                # Referenced UDT not present in this schema snapshot: keep the
                # name for alignment, but decode nothing (opaque).
                try:
                    span = int(f.get("struct_size", 0) or 0)
                except (TypeError, ValueError):
                    span = 0
                self.field_meta_list.append({
                    "full_name": full_name,
                    "db_num": db_num,
                    "db_name": db_name,
                    "field_name": path,
                    "offset": 0,
                    "bit": 0,
                    "type_str": f.get("type", "STRUCT"),
                    "fmt_char": None,
                    "item_size": span,
                    "kind": "opaque",
                    "endian": db_endian or ">",
                    "enum": [],
                    "unit": f.get("unit", ""),
                })
                continue
            count = 0
            try:
                count = int(f.get("count", 1) or 1)
            except (TypeError, ValueError):
                count = 1
            try:
                own_offset = int(f.get("offset", 0) or 0)
            except (TypeError, ValueError):
                own_offset = 0
            if sub_fields:
                if count > 1 or from_udt:
                    try:
                        span = int(f.get("struct_size", 0) or 0)
                    except (TypeError, ValueError):
                        span = 0
                    if not span and isinstance(udt, dict):
                        try:
                            span = int(udt.get("size_bytes", 0) or 0)
                        except (TypeError, ValueError):
                            span = 0
                    stride = (span // count) if span >= count > 0 else 0
                    n = count if count > 1 else 1
                    for i in range(n):
                        elem_prefix = f"{path}[{i}]." if count > 1 else path + "."
                        self._parseFieldList(db_name, db_num, db_endian, sub_fields, udts,
                                             prefix=elem_prefix,
                                             base_offset=own_offset + i * stride)
                else:
                    # Inline struct members already carry absolute offsets.
                    self._parseFieldList(db_name, db_num, db_endian, sub_fields, udts,
                                         prefix=path + ".")
                continue
            if count > 1:
                _, stride, _ = self._scalarLayout(f.get("type", "REAL"), f)
                for i in range(count):
                    item = dict(f)
                    item["name"] = f"{f['name']}[{i}]"
                    item["count"] = 1  # expanded; must not re-expand on recursion
                    item["offset"] = own_offset + i * stride
                    self._parseFieldList(db_name, db_num, db_endian, [item], udts,
                                         prefix=prefix, base_offset=base_offset)
                continue

            offset = own_offset + base_offset if base_offset is not None else own_offset
            type_str = f.get("type", "REAL")

            # Byte order: field pragma wins, else DB default, else big
            # (legacy behaviour) — matches s7codec encodeDtl/I64 paths.
            endian = db_endian
            for key in ("endianness", "endian", "byte_order"):
                v = f.get(key)
                if isinstance(v, str) and v:
                    endian = "<" if v.lower().startswith("l") else ">"
                    break
            if not endian:
                endian = ">"  # legacy default; preserves old decode

            # DTL/LDTL are timestamps and strings are text — neither is a
            # float. The legacy float matrix carries NaN for them rather
            # than garbage; readPandas() decodes both properly.
            fmt_char, item_size, kind = self._scalarLayout(type_str, f)

            enum_node = f.get("enum")
            if isinstance(enum_node, dict):
                try:
                    order = sorted(enum_node.keys(), key=lambda k: int(k))
                except (TypeError, ValueError):
                    order = sorted(enum_node.keys())
                enum_cats = [str(enum_node[k]) for k in order]
            elif isinstance(enum_node, list):
                enum_cats = [str(c) for c in enum_node]
            else:
                enum_cats = []

            bit = 0
            try:
                bit = int(f.get("bit", f.get("bit_index", 0)) or 0)
            except (TypeError, ValueError):
                bit = 0

            self.field_meta_list.append({
                "full_name": full_name,
                "db_num": db_num,
                "db_name": db_name,
                "field_name": path,
                "offset": offset,
                "bit": bit & 0x7,
                "type_str": type_str,
                "fmt_char": fmt_char,
                "item_size": item_size,
                "kind": kind,
                "endian": endian,
                "enum": enum_cats,
                "unit": f.get("unit", "")
            })

    def getFieldNames(self) -> List[str]:
        return [f["full_name"] for f in self.field_meta_list]

    def iterRows(self, columns: Optional[List[str]] = None):
        """Stream ``(ts_ms, {column: raw_value})`` rows in archive order, O(1) memory.

        Decodes only ``columns`` (default: all fields) to plain Python
        scalars (float/bool/int/str/None); DTL/LDTL arrive as integer
        nanoseconds. Carried-forward state mirrors :meth:`readPandas`.
        """
        self._ensureMeta()
        wanted = list(columns) if columns else self.getFieldNames()
        metas = {m["full_name"]: m for m in self.field_meta_list if m["full_name"] in wanted}
        cur: Dict[str, Any] = {}
        for name in wanted:
            m = metas.get(name, {})
            cur[name] = None if m.get("kind") in ("dtl", "ldtl", "string", "wstring", "opaque") else 0
        from ..dtypes import unpackDtl, unpackLdtl, unpackString

        for ts, db_num, payload in self._iterFrames():
            if not self.field_meta_list:
                continue
            touched = False
            for name in wanted:
                meta = metas.get(name)
                if meta is None:
                    continue
                if meta["db_num"] == db_num or db_num == 0 or len(self.schema_json.get("dbs", {})) == 1:
                    off, size = meta["offset"], meta["item_size"]
                    if off + size > len(payload):
                        continue
                    raw = payload[off : off + size]
                    kind = meta.get("kind", "float")
                    endian = meta.get("endian", ">")
                    try:
                        if kind == "dtl":
                            cur[name] = unpackDtl(raw, t_little_endian=(endian == "<"))
                        elif kind == "ldtl":
                            cur[name] = unpackLdtl(raw, t_little_endian=(endian == "<"))
                        elif kind == "bool":
                            bit = int(meta.get("bit", 0) or 0) & 0x7
                            cur[name] = bool((raw[0] >> bit) & 0x1) if raw else None
                        elif kind in ("string", "wstring"):
                            cur[name] = unpackString(raw, t_wide=(kind == "wstring"))
                        elif kind == "opaque":
                            cur[name] = None
                        else:
                            cur[name] = struct.unpack(endian + (meta.get("fmt_char") or "f"), raw)[0]
                        touched = True
                    except Exception:
                        pass
            if touched:
                yield ts, dict(cur)
        # NOTE: rows with zero touched fields are skipped; the anchor framing
        # guarantees the first row touches everything.

    def _ensureMeta(self) -> None:
        # If field_meta_list is empty, try loading SCL schema if provided
        if not self.field_meta_list and self.scl_schema_path and os.path.exists(self.scl_schema_path):
            self._loadSclSchemaFile(self.scl_schema_path)

    def _iterFrames(self):
        """Yield ``(ts_ms, db_num, payload)`` data frames across all archives.

        Owns decompression, SGRN header checks, embedded-schema adoption and
        anchor/delta/control framing with CRC-verified resync. Frame ``ts``
        is milliseconds since the Unix epoch (``PersistenceService`` stamps
        ``nowMilliseconds()``). Shared by :meth:`readDataset` and
        :meth:`readPandas` so both see identical rows.
        """
        # Last full DB image per DB number (v2 delta frames patch these),
        # carried across files like the gateway's own replay.
        last_images: Dict[int, bytearray] = {}
        for file_path in self.files:
            try:
                raw = self._decompressFile(file_path)
            except Exception:
                continue

            if len(raw) < 10 or raw[:4] != b'SGRN':
                continue

            ver, schema_len = struct.unpack('<HI', raw[4:10])
            if ver < 1 or ver > BINARY_WAL_VERSION:
                continue  # unsupported framing; readers must refuse, not misparse
            pos = 10 + schema_len

            # If embedded header has valid schema, parse it if not already done
            if not self.field_meta_list and schema_len > 0 and 10 + schema_len <= len(raw):
                try:
                    schema_str = raw[10:10+schema_len].decode('utf-8', errors='ignore')
                    self.schema_json = json.loads(schema_str)
                    self._parseFieldMeta()
                except Exception:
                    pass

            while pos + 14 <= len(raw):
                frame_start = pos
                ts, db_num, payload_len = struct.unpack('<QHI', raw[pos:pos+14])
                pos += 14

                if pos + payload_len > len(raw):
                    found = _findNextAnchor(raw, frame_start)
                    if found is None:
                        break
                    pos = found
                    continue

                payload = raw[pos : pos + payload_len]
                pos += payload_len

                # Control frames (dictionary / manifest / anchor / footer)
                # carry JSON, not raw DB memory — skip before field decode.
                if db_num == CONTROL_FRAME_DB_NUM:
                    continue

                # Anchor frames (v3+): db + crc32 + full image. Verify before
                # trusting a byte; a mismatch seeks resync, never adopts.
                if db_num == ANCHOR_FRAME_DB_NUM:
                    anchor_ok = False
                    if len(payload) >= 6:
                        anchor_db, anchor_crc = struct.unpack('<HI', payload[:6])
                        if zlib.crc32(payload[6:]) & 0xFFFFFFFF == anchor_crc:
                            last_images[anchor_db] = bytearray(payload[6:])
                            db_num = anchor_db
                            payload = bytes(last_images[anchor_db])
                            anchor_ok = True
                    if not anchor_ok:
                        found = _findNextAnchor(raw, frame_start)
                        if found is None:
                            break
                        pos = found
                        continue

                # Delta frames (v2+): patch (offset,len,bytes) runs into the
                # cached image for the embedded DB, then decode from the image.
                elif db_num == DELTA_FRAME_DB_NUM:
                    if len(payload) < 2:
                        continue
                    (delta_db,) = struct.unpack('<H', payload[:2])
                    image = last_images.get(delta_db)
                    if image is None:
                        continue  # no keyframe yet
                    rp = 2
                    ok = True
                    corrupt = False
                    while rp + 8 <= len(payload):
                        off, ln = struct.unpack('<II', payload[rp:rp + 8])
                        rp += 8
                        if rp + ln > len(payload) or off + ln > len(image):
                            ok = False
                            corrupt = True
                            break
                        image[off:off + ln] = payload[rp:rp + ln]
                        rp += ln
                    if rp != len(payload):
                        ok = False
                        corrupt = True
                    if not ok:
                        if not corrupt:
                            continue
                        found = _findNextAnchor(raw, frame_start)
                        if found is None:
                            break
                        pos = found
                        continue
                    db_num = delta_db
                    payload = bytes(image)

                last_images[db_num] = bytearray(payload)
                yield int(ts), db_num, bytes(last_images[db_num])

    def _decodeNumeric(self, meta: Dict[str, Any], val_bytes: bytes) -> Tuple[bool, float]:
        """Decode one field slice to float for the legacy matrix.

        Returns ``(ok, value)``. Bit-packed BOOLs are masked by bit index;
        DTL/LDTL timestamps and strings/opaque blobs are not representable
        and yield ``(False, nan)`` — use :meth:`readPandas` for them instead
        of inheriting garbage floats.
        """
        kind = meta.get("kind", "float")
        if kind in ("dtl", "ldtl", "string", "wstring", "opaque"):
            return False, float("nan")
        if kind == "bool":
            if not val_bytes:
                return False, float("nan")
            bit = int(meta.get("bit", 0) or 0) & 0x7
            return True, float((val_bytes[0] >> bit) & 0x1)
        fmt = meta.get("endian", ">") + (meta.get("fmt_char") or "f")
        try:
            return True, float(struct.unpack(fmt, val_bytes)[0])
        except Exception:
            return False, float("nan")

    def readDataset(self) -> Tuple[Any, List[str], Any]:
        """
        Parses all binary archives and returns:
        - timestamps: np.ndarray shape (N,) int64 milliseconds since epoch
        - feature_names: List[str] length F
        - data_matrix: np.ndarray shape (N, F) float32

        Numeric fields decode with their schema byte order. DTL/LDTL
        timestamp fields are not representable here and read as NaN — use
        :meth:`readPandas` for real timestamps instead of the CSV round-trip.
        """
        if np is None:
            raise ImportError("numpy library is required for readDataset()")

        self._ensureMeta()

        feature_names = self.getFieldNames()
        all_timestamps = []
        rows = []

        current_state = np.zeros(len(feature_names) if feature_names else 1, dtype=np.float32)
        for _i, _m in enumerate(self.field_meta_list):
            if _m.get("kind") in ("dtl", "ldtl", "string", "wstring", "opaque"):
                current_state[_i] = np.nan  # decoded by readPandas(), not floats

        for ts, db_num, payload in self._iterFrames():
            if not self.field_meta_list:
                # No schema fields: record a dummy slot so records are counted.
                if not rows:
                    all_timestamps.append(ts)
                    rows.append(current_state.copy())
                continue
            if len(feature_names) != len(self.field_meta_list):
                feature_names = self.getFieldNames()
                current_state = np.zeros(len(feature_names), dtype=np.float32)
                for _i, _m in enumerate(self.field_meta_list):
                    if _m.get("kind") in ("dtl", "ldtl", "string", "wstring", "opaque"):
                        current_state[_i] = np.nan

            updated = False
            for meta_idx, meta in enumerate(self.field_meta_list):
                if meta["db_num"] == db_num or db_num == 0 or len(self.schema_json.get("dbs", {})) == 1:
                    off = meta["offset"]
                    size = meta["item_size"]
                    if off + size <= len(payload):
                        ok, val = self._decodeNumeric(meta, payload[off : off + size])
                        if ok:
                            current_state[meta_idx] = val
                            updated = True
                        # Timestamp kinds (dtl/ldtl) intentionally untouched:
                        # NaN until readPandas() decodes them properly.

            if updated or len(rows) == 0:
                all_timestamps.append(ts)
                rows.append(current_state.copy())

        if not rows:
            return np.array([]), feature_names, np.zeros((0, len(feature_names)), dtype=np.float32)

        return np.array(all_timestamps, dtype=np.int64), feature_names, np.array(rows, dtype=np.float32)

    def readPandas(self, timestamp_col: str = "timestamp"):
        """Decode archives straight into a DataFrame — no CSV intermediate.

        Columns carry faithful dtypes (float32/float64 numerics, bool,
        int32 codes, datetime64[ns] for DTL/LDTL) plus a ``timestamp``
        datetime64[ns] column from the frame clock (ms). This replaces the
        ``sgrn_dataset`` CSV + :class:`DatasetReader` round-trip, which
        stringifies floats and maps ``true``/``false`` by string compare.
        """
        if np is None:
            raise ImportError("numpy library is required for readPandas()")
        if pd is None:
            raise ImportError("pandas library is required for readPandas()")
        from ..dtypes import unpackDtl, unpackLdtl, unpackString

        self._ensureMeta()

        feature_names = self.getFieldNames()
        ts_list: List[int] = []
        cols: Dict[str, list] = {name: [] for name in feature_names}
        cur: Dict[str, Any] = {}
        for meta in self.field_meta_list:
            kind = meta.get("kind", "float")
            cur[meta["full_name"]] = None if kind in ("dtl", "ldtl", "string", "wstring", "opaque") else 0

        for ts, db_num, payload in self._iterFrames():
            if not self.field_meta_list:
                continue
            touched = False
            for meta in self.field_meta_list:
                if meta["db_num"] == db_num or db_num == 0 or len(self.schema_json.get("dbs", {})) == 1:
                    off, size = meta["offset"], meta["item_size"]
                    if off + size > len(payload):
                        continue
                    raw = payload[off : off + size]
                    kind = meta.get("kind", "float")
                    endian = meta.get("endian", ">")
                    try:
                        if kind == "dtl":
                            cur[meta["full_name"]] = unpackDtl(raw, t_little_endian=(endian == "<"))
                        elif kind == "ldtl":
                            cur[meta["full_name"]] = unpackLdtl(raw, t_little_endian=(endian == "<"))
                        elif kind == "bool":
                            bit = int(meta.get("bit", 0) or 0) & 0x7
                            cur[meta["full_name"]] = bool((raw[0] >> bit) & 0x1) if raw else None
                        elif kind in ("string", "wstring"):
                            cur[meta["full_name"]] = unpackString(
                                raw, t_wide=(kind == "wstring"))
                        elif kind == "opaque":
                            cur[meta["full_name"]] = None  # undecodable here
                        elif kind == "opaque":
                            cur[meta["full_name"]] = None
                        else:
                            fmt = endian + (meta.get("fmt_char") or "f")
                            cur[meta["full_name"]] = struct.unpack(fmt, raw)[0]
                        touched = True
                    except Exception:
                        pass
            if touched or not ts_list:
                ts_list.append(ts)
                for name in feature_names:
                    cols[name].append(cur[name])

        df = pd.DataFrame({timestamp_col: pd.to_datetime(ts_list, unit="ms")})
        for meta in self.field_meta_list:
            name = meta["full_name"]
            kind = meta.get("kind", "float")
            values = cols[name]
            if kind == "bool":
                df[name] = pd.Series(values, dtype="bool")
            elif kind == "int":
                df[name] = pd.Series(values, dtype="int32")
            elif kind in ("dtl", "ldtl"):
                df[name] = pd.to_datetime(pd.Series(values, dtype="Int64"), unit="ns")
            elif kind in ("string", "wstring"):
                df[name] = pd.Series(values, dtype="object")
            elif kind == "opaque":
                df[name] = pd.Series(values, dtype="object")
            elif meta.get("fmt_char") == "d":
                df[name] = pd.Series(values, dtype="float64")
            else:
                df[name] = pd.Series(values, dtype="float32")
        return df


class JsonlDatasetReader:
    """Direct reader for gateway JSONL replay archives (``.jsonl.zst`` / ``.jsonl``).

    Mirrors the JSONL WAL LINE CONTRACT in
    ``sgrn/lib/gateway/src/database/PersistenceService.cpp`` (schema /
    dictionary / manifest / anchor / delta / footer lines; dictionary-mode
    numeric leaf IDs; nested and legacy ``{"db","path","val"}`` shapes) and
    the carried-forward row semantics of ``sgrn_dataset``: one DataFrame row
    per timestamped record from the running state — no CSV intermediate.
    """

    def __init__(self, file_or_dir_path: str, manifest_path: Optional[str] = None):
        self.path = file_or_dir_path
        self.files: List[str] = []
        self.schema_json: Dict[str, Any] = {}
        self.manifest: Dict[str, Any] = {}
        self._discoverFiles()
        if manifest_path and os.path.exists(manifest_path):
            with open(manifest_path, "r", encoding="utf-8") as f:
                self.manifest = json.load(f)

    def _discoverFiles(self) -> None:
        if os.path.isfile(self.path):
            if self.path.endswith((".jsonl.zst", ".jsonl", ".zst")):
                self.files.append(self.path)
        elif os.path.isdir(self.path):
            for root, _, files in os.walk(self.path):
                for f in files:
                    if f.endswith((".jsonl.zst", ".jsonl")) or (
                        f.endswith(".zst") and ".jsonl" not in f
                        and not f.endswith(".bin.zst")
                    ):
                        self.files.append(os.path.join(root, f))
            self.files.sort()
        if not self.files:
            raise FileNotFoundError(f"No JSONL archive files found under path: {self.path}")

    def _readText(self, file_path: str) -> str:
        if file_path.endswith(".zst"):
            import subprocess
            try:
                res = subprocess.run(["zstd", "-d", "-c", file_path],
                                     capture_output=True, check=True)
                return res.stdout.decode("utf-8", errors="ignore")
            except Exception:
                pass
            if zstd is not None:
                with open(file_path, "rb") as f:
                    blob = f.read()
                if hasattr(zstd, "ZstdDecompressor"):
                    dctx = zstd.ZstdDecompressor()
                    try:
                        reader = dctx.stream_reader(blob)
                        chunks = []
                        while True:
                            chunk = reader.read(1 << 20)
                            if not chunk:
                                break
                            chunks.append(chunk)
                        return b"".join(chunks).decode("utf-8", errors="ignore")
                    except Exception:
                        pass
                raise RuntimeError(f"Failed to decompress {file_path}")
        with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read()

    @staticmethod
    def _walkNested(prefix: str, node: Any, state: Dict[str, Any]) -> None:
        if isinstance(node, dict):
            for key, val in node.items():
                JsonlDatasetReader._walkNested(f"{prefix}.{key}" if prefix else str(key), val, state)
        elif isinstance(node, list):
            for i, val in enumerate(node):
                JsonlDatasetReader._walkNested(f"{prefix}[{i}]", val, state)
        elif node is None or isinstance(node, (str, int, float, bool)):
            state[prefix] = node

    def _ingestKeyed(self, obj: Dict[str, Any], path_by_id: List[str], state: Dict[str, Any]) -> None:
        for key, val in obj.items():
            if path_by_id and key.isdigit():
                idx = int(key)
                if 0 <= idx < len(path_by_id) and path_by_id[idx]:
                    key = path_by_id[idx]
            self._walkNested(key, val, state)

    def _ingestRecord(self, doc: Dict[str, Any], path_by_id: List[str],
                      state: Dict[str, Any]) -> Optional[int]:
        """Ingest one line; returns ts_ms when it produces a row, else None."""
        dtype = doc.get("type")
        if dtype == "schema":
            schema = doc.get("schema")
            if isinstance(schema, dict) and schema.get("dbs"):
                self.schema_json = schema
            return None
        if dtype == "dictionary":
            leaves = doc.get("leaves", [])
            by_id: Dict[int, str] = {}
            for leaf in leaves:
                if isinstance(leaf, dict) and "id" in leaf and "path" in leaf:
                    try:
                        by_id[int(leaf["id"])] = str(leaf["path"])
                    except (TypeError, ValueError):
                        pass
            path_by_id.clear()
            if by_id:
                path_by_id.extend(by_id[i] for i in range(max(by_id) + 1) if i in by_id)
            return None
        if dtype in ("manifest", "footer"):
            return None

        ts = doc.get("ts", doc.get("timestamp", 0)) or 0
        try:
            ts = int(ts)
        except (TypeError, ValueError):
            return None
        if not ts:
            return None

        if isinstance(doc.get("changes"), dict):
            self._ingestKeyed(doc["changes"], path_by_id, state)
        elif isinstance(doc.get("data"), dict):
            data = doc["data"]
            if any(isinstance(v, dict) for v in data.values()):
                for db_name, subtree in data.items():
                    if isinstance(subtree, dict):
                        self._walkNested(str(db_name), subtree, state)
            else:
                self._ingestKeyed(data, path_by_id, state)
        elif "db" in doc and "path" in doc and "val" in doc:
            self._walkNested(f"{doc['db']}.{doc['path']}", doc["val"], state)
        else:
            # Bare nested {"Db": {...}} or bare id-keyed {"0": v} record.
            handled_nested = False
            for key, val in doc.items():
                if key in ("type", "ts", "timestamp"):
                    continue
                if isinstance(val, (dict, list)):
                    self._walkNested(str(key), val, state)
                    handled_nested = True
            if not handled_nested and all(
                str(k).isdigit() for k in doc.keys()
                if k not in ("type", "ts", "timestamp")
            ):
                self._ingestKeyed(
                    {k: v for k, v in doc.items() if k not in ("type", "ts", "timestamp")},
                    path_by_id, state,
                )
        return ts

    def _iterRows(self):
        """Yield ``(ts_ms, state_snapshot)`` records; state carries forward."""
        state: Dict[str, Any] = {}
        for file_path in self.files:
            text = self._readText(file_path)
            if text.startswith("SGRN"):
                raise ValueError(
                    f"{file_path} is a binary WAL archive, not JSONL — "
                    "use BinaryDatasetReader for it."
                )
            path_by_id: List[str] = []  # file-local, like the gateway replay
            for line in text.splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    doc = json.loads(line)
                except Exception:
                    continue  # corrupt line: skip, never misparse
                if not isinstance(doc, dict):
                    continue
                ts = self._ingestRecord(doc, path_by_id, state)
                if ts:
                    yield ts, dict(state)

    def getFieldNames(self) -> List[str]:
        names: List[str] = []
        seen = set()
        for _, state in self._iterRows():
            for key in state:
                if key not in seen:
                    seen.add(key)
                    names.append(key)
        return names

    def iterRows(self, columns: Optional[List[str]] = None):
        """Stream ``(ts_ms, {column: raw_value})`` rows in archive order, O(1) memory."""
        wanted = set(columns) if columns else None
        for ts, state in self._iterRows():
            yield ts, {k: v for k, v in state.items() if wanted is None or k in wanted}

    @staticmethod
    def _inferDtype(values: List[Any]) -> str:
        present = [v for v in values if v is not None]
        if not present:
            return "float32"
        if all(isinstance(v, bool) for v in present):
            return "bool" if len(present) == len(values) else "boolean"
        if all(isinstance(v, int) for v in present):
            ints = [int(v) for v in present]
            if all(-2**31 <= v < 2**31 for v in ints):
                return "int32" if len(present) == len(values) else "Int32"
            return "int64" if len(present) == len(values) else "Int64"
        if not any(isinstance(v, bool) for v in present):
            try:
                [float(v) for v in present]
                return "float32"  # ints, floats and numeric strings alike
            except (TypeError, ValueError):
                pass
        if all(isinstance(v, str) for v in present):
            try:
                pd.to_datetime(pd.Series(present))
                return "datetime64"
            except Exception:
                pass
            try:
                [float(v) for v in present]
                return "float32"
            except (TypeError, ValueError):
                pass
            return "object"
        return "object"

    def readPandas(self, timestamp_col: str = "timestamp"):
        """Decode archives straight into a DataFrame — no CSV intermediate."""
        if pd is None:
            raise ImportError("pandas library is required for readPandas()")
        names: List[str] = []
        seen = set()
        ts_list: List[int] = []
        cols: Dict[str, list] = {}
        for ts, state in self._iterRows():
            for key in state:
                if key not in seen:
                    seen.add(key)
                    names.append(key)
                    cols[key] = [None] * len(ts_list)
            ts_list.append(ts)
            for key in names:
                cols[key].append(state.get(key))
        df = pd.DataFrame({timestamp_col: pd.to_datetime(ts_list, unit="ms")})
        for name in names:
            kind = self._inferDtype(cols[name])
            if kind == "datetime64":
                df[name] = pd.to_datetime(pd.Series(cols[name]), errors="coerce").astype("datetime64[ns]")
            elif kind in ("bool", "boolean", "int32", "Int32", "int64", "Int64"):
                df[name] = pd.Series(cols[name], dtype=kind)
            elif kind == "float32":
                df[name] = pd.to_numeric(pd.Series(cols[name]), errors="coerce").astype("float32")
            else:
                df[name] = pd.Series(cols[name], dtype="object")
        return df

    def readDataset(self) -> Tuple[Any, List[str], Any]:
        """Legacy ``(timestamps_ms, names, float32 matrix)`` view over :meth:`readPandas`.

        Mirrors the ``sgrn_dataset`` CSV numeric mapping (``true``→1,
        unparseable→NaN); timestamps stay out of the matrix.
        """
        if np is None:
            raise ImportError("numpy library is required for readDataset()")
        df = self.readPandas()
        names = [c for c in df.columns if c != "timestamp"]
        ts = (df["timestamp"].astype("int64") // 1_000_000).to_numpy(dtype=np.int64)
        mat = np.empty((len(df), len(names)), dtype=np.float32)
        for j, name in enumerate(names):
            col = df[name]
            if pd.api.types.is_bool_dtype(col.dtype):
                mat[:, j] = col.to_numpy(dtype=np.float32)
            elif pd.api.types.is_numeric_dtype(col.dtype):
                mat[:, j] = pd.to_numeric(col, errors="coerce").to_numpy(dtype=np.float32)
            elif pd.api.types.is_datetime64_any_dtype(col.dtype):
                mat[:, j] = np.nan
            else:
                mat[:, j] = pd.to_numeric(col, errors="coerce").to_numpy(dtype=np.float32)
        return ts, names, mat
