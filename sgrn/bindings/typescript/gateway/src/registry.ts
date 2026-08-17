import type {
  DbField,
  DbSchema,
  EipProjectionRow,
  FlatRegistryField,
  OpcuaProjectionRow,
  RegistryResponse,
  RegistryTreeBuildResult,
  RegistryTreeNode,
  SymbolTag,
  UdtSchema,
} from "./types";

function resolveChildren(t_field: DbField, t_registry: RegistryResponse): DbField[] | undefined {
  let children = t_field.children;
  if (!children && (t_field.type.startsWith("UDT") || t_field.udt_name)) {
    const udt = t_registry.udts?.find((u) => u.name === (t_field.udt_name || t_field.type));
    if (udt) {
      children = udt.fields;
    }
  }
  return children;
}

export function buildRegistryTree(
  t_registry: RegistryResponse,
  t_render_limit_rows = 64,
): RegistryTreeBuildResult {
  const nodes: RegistryTreeNode[] = [];
  const node_map = new Map<string, RegistryTreeNode>();
  const db_names: string[] = [];

  function walkField(
    t_db: DbSchema,
    t_field: DbField,
    t_parent_path: string,
    t_depth: number,
    t_parent_row_id: string,
  ): void {
    const full_path = t_parent_path ? `${t_parent_path}/${t_field.name}` : t_field.name;
    const key = `${t_db.db_name}-${full_path}`;
    const children = resolveChildren(t_field, t_registry);
    const is_struct = !!(children && children.length > 0);
    const is_array =
      t_field.count > 1 &&
      t_field.type !== "STRING" &&
      t_field.type !== "WSTRING";
    const safe_key = key.replace(/[\[\]./]/g, "_");
    const row_id = `row-${safe_key}`;

    const node: RegistryTreeNode = {
      id: row_id,
      type: "field",
      db_num: t_db.db_number,
      db_name: t_db.db_name,
      name: t_field.name,
      path: full_path,
      key,
      depth: t_depth,
      is_struct,
      is_array,
      field_type: t_field.type,
      parent_row_id: t_parent_row_id,
      count: t_field.count,
      unit: t_field.unit,
      min: t_field.min,
      max: t_field.max,
      enum_map: t_field.enum,
    };
    nodes.push(node);
    if (node.key) {
      node_map.set(node.key, node);
    }

    if (is_struct) {
      for (const child of children ?? []) {
        walkField(t_db, child, full_path, t_depth + 1, row_id);
      }
      return;
    }

    if (is_array) {
      const render_limit = Math.min(t_field.count, t_render_limit_rows);
      for (let i = 0; i < render_limit; i++) {
        const sub_field: DbField = { ...t_field, name: `[${i}]`, count: 1 };
        walkField(t_db, sub_field, full_path, t_depth + 1, row_id);
      }
      if (t_field.count > render_limit) {
        nodes.push({
          id: `${row_id}-truncated`,
          type: "field",
          db_num: t_db.db_number,
          db_name: t_db.db_name,
          name: `Truncated: ${t_field.count - render_limit} more elements...`,
          path: "",
          key: "",
          depth: t_depth + 1,
          is_struct: false,
          is_array: false,
          field_type: "TRUNCATED",
          parent_row_id: row_id,
          count: 0,
        });
      }
    }
  }

  for (const db of t_registry.dbs ?? []) {
    db_names.push(db.db_name);
    const node: RegistryTreeNode = {
      id: `db-${db.db_number}`,
      type: "db",
      db_num: db.db_number,
      db_name: db.db_name,
      name: db.db_name,
      path: "",
      key: "",
      depth: 0,
      is_struct: true,
      is_array: false,
      field_type: "DB",
      parent_row_id: null,
      count: 0,
    };
    nodes.push(node);

    for (const field of db.fields ?? []) {
      walkField(db, field, "", 1, node.id);
    }
  }

  return { nodes, node_map, db_names };
}

export function flattenRegistryFields(
  t_fields: DbField[] | undefined,
  t_registry: RegistryResponse,
  t_depth = 0,
  t_parent_path = "",
): FlatRegistryField[] {
  if (!t_fields) return [];
  const results: FlatRegistryField[] = [];
  for (const field of t_fields) {
    const offset = field.bit_index > 0 ? `${field.offset}.${field.bit_index}` : `${field.offset}.0`;
    const full_path = t_parent_path ? `${t_parent_path}/${field.name}` : field.name;
    results.push({
      name: field.name,
      type: field.type,
      udt_name: field.udt_name || "",
      offset,
      count: field.count,
      depth: t_depth,
      full_path,
      unit: field.unit,
      min: field.min,
      max: field.max,
      enum_map: field.enum,
    });

    const children = resolveChildren(field, t_registry);
    if (children) {
      results.push(...flattenRegistryFields(children, t_registry, t_depth + 1, full_path));
    }
  }
  return results;
}

export function filterDbs(t_dbs: DbSchema[], t_term: string): DbSchema[] {
  if (!t_term) return t_dbs;
  return t_dbs.filter((db) => {
    const db_match =
      db.db_name.toLowerCase().includes(t_term) || `db${db.db_number}`.includes(t_term);
    const field_match =
      db.fields?.some((field) => field.name.toLowerCase().includes(t_term)) ?? false;
    return db_match || field_match;
  });
}

export function filterUdts(t_udts: UdtSchema[], t_term: string): UdtSchema[] {
  if (!t_term) return t_udts;
  return t_udts.filter((udt) => udt.name.toLowerCase().includes(t_term));
}

export function filterTags(t_tags: SymbolTag[], t_term: string): SymbolTag[] {
  if (!t_term) return t_tags;
  return t_tags.filter(
    (tag) =>
      tag.name.toLowerCase().includes(t_term) ||
      tag.address.toLowerCase().includes(t_term) ||
      tag.type.toLowerCase().includes(t_term),
  );
}

export function getCipType(s7Type: string): string {
  const t = s7Type.toUpperCase();
  if (t === "BOOL") return "BOOL (0xC1)";
  if (t === "BYTE" || t === "USINT" || t === "CHAR") return "USINT (0xC6)";
  if (t === "SINT") return "SINT (0xC2)";
  if (t === "WORD" || t === "UINT") return "UINT (0xC7)";
  if (t === "INT") return "INT (0xC3)";
  if (t === "DWORD" || t === "UDINT" || t === "TIME" || t === "TOD") return "UDINT (0xC8)";
  if (t === "DINT") return "DINT (0xC4)";
  if (t === "REAL") return "REAL (0xCA)";
  if (t === "LWORD" || t === "ULINT" || t === "LTIME" || t === "LTOD") return "ULINT (0xC9)";
  if (t === "LINT") return "LINT (0xC5)";
  if (t === "LREAL") return "LREAL (0xCB)";
  return "BYTE_ARRAY (0xD3)";
}

export function buildEipProjection(t_registry: RegistryResponse): EipProjectionRow[] {
  const rows: EipProjectionRow[] = [];
  for (const db of t_registry.dbs ?? []) {
    let attr = 1;
    for (const field of db.fields ?? []) {
      const isComposite =
        field.count > 1 ||
        (field.children && field.children.length > 0) ||
        field.type.includes("String");
      rows.push({
        db: db.db_name,
        instance: db.db_number,
        attr: attr++,
        field: field.name,
        cipType: isComposite ? "BYTE_ARRAY (0xD3)" : getCipType(field.type),
      });
    }
  }
  return rows;
}

export function buildOpcuaProjection(t_registry: RegistryResponse): OpcuaProjectionRow[] {
  const rows: OpcuaProjectionRow[] = [];

  for (const db of t_registry.dbs ?? []) {
    rows.push({
      db: db.db_name,
      nodeId: `ns=1;s=${db.db_name}`,
      dataType: "FolderType",
      valueRank: "Scalar",
    });

    const walkFields = (fields: DbField[], parentPath: string): void => {
      for (const field of fields) {
        const fullPath = parentPath ? `${parentPath}.${field.name}` : field.name;
        const isArray = field.count > 1 || field.type.includes("Array");
        rows.push({
          db: db.db_name,
          nodeId: `ns=1;s=${db.db_name}.${fullPath}.Value`,
          dataType: field.udt_name ? field.udt_name : field.type,
          valueRank: isArray ? "OneDimension" : "Scalar",
        });
        if (field.children && field.children.length > 0) {
          walkFields(field.children, fullPath);
        }
      }
    };

    if (db.fields) {
      walkFields(db.fields, "");
    }
  }

  return rows;
}
