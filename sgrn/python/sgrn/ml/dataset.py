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


def _find_next_anchor(raw: bytes, frm: int):
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
        self._load_manifest()

    def _load_manifest(self) -> None:
        if not os.path.exists(self.manifest_path):
            raise FileNotFoundError(f"Manifest not found: {self.manifest_path}")
        with open(self.manifest_path, "r", encoding="utf-8") as f:
            self.manifest = json.load(f)
        self.features = self.manifest.get("features", [])

    def get_feature_names(self, categorical_only: bool = False, continuous_only: bool = False) -> List[str]:
        names = []
        for feat in self.features:
            if categorical_only and not feat.get("is_categorical", False):
                continue
            if continuous_only and feat.get("is_categorical", False):
                continue
            names.append(feat["name"])
        return names

    def load_pandas(self) -> Optional["pd.DataFrame"]:
        if pd is None:
            raise ImportError("pandas library is required for load_pandas()")
        
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

    def load_numpy(self, target_column: Optional[str] = None) -> Tuple[Any, Optional[Any]]:
        if np is None:
            raise ImportError("numpy library is required for load_numpy()")
        
        df = self.load_pandas()
        
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

        self._discover_files()
        if self.files:
            self._load_header_schema(self.files[0])

    def _discover_files(self) -> None:
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

    def _decompress_file(self, file_path: str) -> bytes:
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

    def _load_header_schema(self, file_path: str) -> None:
        raw = self._decompress_file(file_path)
        if len(raw) < 10 or raw[:4] != b'SGRN':
            raise ValueError(f"File {file_path} is not a valid SGRN binary archive (missing 'SGRN' magic header)")
        
        ver, schema_len = struct.unpack('<HI', raw[4:10])
        if schema_len > 0 and 10 + schema_len <= len(raw):
            schema_str = raw[10:10+schema_len].decode('utf-8', errors='ignore')
            try:
                self.schema_json = json.loads(schema_str)
                self._parse_field_meta()
            except Exception:
                pass
        
        # If embedded header schema was empty and scl_schema_path was provided, load SCL schema file
        if not self.field_meta_list and self.scl_schema_path and os.path.exists(self.scl_schema_path):
            self._load_scl_schema_file(self.scl_schema_path)

    def _load_scl_schema_file(self, scl_path: str) -> None:
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
                    "unit": ""
                })
                offset += item_size
            default_db_num += 1

    def _parse_field_meta(self) -> None:
        self.field_meta_list.clear()
        dbs = self.schema_json.get("dbs", {})
        if isinstance(dbs, list):
            db_dict = {}
            for db in dbs:
                db_dict[db.get("db_number", 0)] = db
            dbs = db_dict

        for db_key, db_val in dbs.items():
            db_num = int(db_key) if isinstance(db_key, (int, str)) and str(db_key).isdigit() else db_val.get("db_number", 0)
            db_name = db_val.get("db_name", f"DB{db_num}")
            fields = db_val.get("fields", [])
            for f in fields:
                field_name = f.get("name", "")
                full_name = f"{db_name}.{field_name}"
                offset = f.get("offset", 0)
                type_str = f.get("type", "REAL")
                
                # Deduce byte length and format
                fmt_char = 'f' # default float32
                item_size = 4
                if type_str in ["REAL", "Single", "Float"]:
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
                    "full_name": full_name,
                    "db_num": db_num,
                    "db_name": db_name,
                    "field_name": field_name,
                    "offset": offset,
                    "type_str": type_str,
                    "fmt_char": fmt_char,
                    "item_size": item_size,
                    "unit": f.get("unit", "")
                })

    def get_field_names(self) -> List[str]:
        return [f["full_name"] for f in self.field_meta_list]

    def read_dataset(self) -> Tuple[np.ndarray, List[str], np.ndarray]:
        """
        Parses all binary archives and returns:
        - timestamps: np.ndarray shape (N,)
        - feature_names: List[str] length F
        - data_matrix: np.ndarray shape (N, F)
        """
        if np is None:
            raise ImportError("numpy library is required for read_dataset()")

        # If field_meta_list is empty, try loading SCL schema if provided
        if not self.field_meta_list and self.scl_schema_path and os.path.exists(self.scl_schema_path):
            self._load_scl_schema_file(self.scl_schema_path)

        feature_names = self.get_field_names()
        all_timestamps = []
        rows = []

        current_state = np.zeros(len(feature_names) if feature_names else 1, dtype=np.float32)
        # Last full DB image per DB number (v2 delta frames patch these).
        last_images: Dict[int, bytearray] = {}

        for file_path in self.files:
            try:
                raw = self._decompress_file(file_path)
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
                    self._parse_field_meta()
                    feature_names = self.get_field_names()
                    current_state = np.zeros(len(feature_names), dtype=np.float32)
                except Exception:
                    pass
            
            while pos + 14 <= len(raw):
                frame_start = pos
                ts, db_num, payload_len = struct.unpack('<QHI', raw[pos:pos+14])
                pos += 14

                if pos + payload_len > len(raw):
                    found = _find_next_anchor(raw, frame_start)
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
                        found = _find_next_anchor(raw, frame_start)
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
                        found = _find_next_anchor(raw, frame_start)
                        if found is None:
                            break
                        pos = found
                        continue
                    db_num = delta_db
                    payload = bytes(image)

                last_images[db_num] = bytearray(payload)
                payload = bytes(last_images[db_num])

                updated = False
                if self.field_meta_list:
                    for meta_idx, meta in enumerate(self.field_meta_list):
                        if meta["db_num"] == db_num or db_num == 0 or len(self.schema_json.get("dbs", {})) == 1:
                            off = meta["offset"]
                            size = meta["item_size"]
                            if off + size <= len(payload):
                                val_bytes = payload[off : off + size]
                                fmt = '>' + meta["fmt_char"]
                                try:
                                    val = struct.unpack(fmt, val_bytes)[0]
                                    current_state[meta_idx] = float(val)
                                    updated = True
                                except Exception:
                                    pass
                else:
                    # Fallback if no schema fields: record dummy slot so records are counted
                    updated = True

                if updated or len(rows) == 0:
                    all_timestamps.append(ts)
                    rows.append(current_state.copy())

        if not rows:
            return np.array([]), feature_names, np.zeros((0, len(feature_names)), dtype=np.float32)

        return np.array(all_timestamps, dtype=np.int64), feature_names, np.array(rows, dtype=np.float32)
