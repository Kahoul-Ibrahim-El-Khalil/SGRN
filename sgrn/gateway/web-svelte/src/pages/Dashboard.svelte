<script lang="ts">
  import { onMount } from "svelte";
  import {
    liveTelemetryValues,
    sendWorkerCommand,
  } from "../lib/telemetryStore";
  import {
    buildRegistryTree,
    fetchFullRegistry,
    filterDbs,
    filterTags,
    filterUdts,
    flattenRegistryFields,
    type RegistryResponse,
    type RegistryTreeNode,
  } from "@sgrn/gateway";

  let active_tab: "process" | "registry" = "process";
  let search_query: string = "";
  let registry_search_query: string = "";
  let registry: RegistryResponse | null = null;
  let loading_registry: boolean = true;
  let registry_error: string = "";
  let show_only_subscribed: boolean = false;

  // Telemetry flat cell map: key → { val, sync }
  let cell_map: Map<string, { val: string; sync: string }> = new Map<
    string,
    { val: string; sync: string }
  >();
  let update_count: number = 0;
  let stream_paused: boolean = false;
  let paused_paths: Set<string> = new Set<string>();

  // Virtualization state
  let scroll_top: number = 0;
  let viewport_height: number = 600;
  const row_height: number = 28;
  const overscan: number = 20;

  // Tree nodes for Process Image
  let tree_nodes: RegistryTreeNode[] = [];
  let expanded_nodes: Set<string> = new Set<string>(); // node IDs that are expanded
  let node_map: Map<string, RegistryTreeNode> = new Map<string, RegistryTreeNode>();
  let subscribed_paths: Set<string> = new Set<string>(); // paths that are active in WS

  // Collapsible sections for Schema Registry
  let expanded_reg_groups: Set<string> = new Set<string>();

  onMount(async () => {
    try {
      registry = await fetchFullRegistry();
      const built = buildRegistryTree(registry);
      tree_nodes = built.nodes;
      node_map = built.node_map;
      subscribed_paths = new Set(built.db_names);

      // Auto-expand DB nodes by default
      for (const node of tree_nodes) {
        if (node.type === "db") {
          expanded_nodes.add(node.id);
        }
      }
      expanded_nodes = new Set(expanded_nodes);

      // Auto-expand all registry group headers by default
      if (registry.dbs) {
        registry.dbs.forEach((_, idx) =>
          expanded_reg_groups.add(`reg-group-db-${idx}`),
        );
      }
      if (registry.udts) {
        registry.udts.forEach((_, idx) =>
          expanded_reg_groups.add(`reg-group-udt-${idx}`),
        );
      }
      if (registry.tags && registry.tags.length > 0) {
        expanded_reg_groups.add("reg-group-tags");
      }
      expanded_reg_groups = new Set(expanded_reg_groups);

      loading_registry = false;
    } catch (e: any) {
      registry_error = e.message;
      loading_registry = false;
    }
  });

  // React to live telemetry updates
  $: if ($liveTelemetryValues && !stream_paused) {
    const vals = $liveTelemetryValues;
    for (const key in vals) {
      const { value, ts } = vals[key];
      const node = node_map.get(key);
      let formatted = "";
      if (
        node?.enum_map &&
        (typeof value === "number" || typeof value === "string")
      ) {
        const enum_str = node.enum_map[String(value)];
        formatted = enum_str ? `${value} (${enum_str})` : String(value);
      } else if (typeof value === "number") {
        formatted = value.toFixed(4);
      } else {
        formatted = String(value ?? "");
      }
      const date = new Date(ts);
      const timeStr =
        date.toLocaleTimeString([], { hour12: false }) +
        "." +
        String(date.getMilliseconds()).padStart(3, "0");
      cell_map.set(key, { val: formatted, sync: timeStr });
      update_count++;
    }
    cell_map = new Map(cell_map);
  }

  // Pre-calculate visible node IDs reactively
  let visible_node_ids: Set<string> = new Set<string>();
  $: {
    const visible: Set<string> = new Set<string>();
    const search: string = search_query.trim().toLowerCase();

    if (search || show_only_subscribed) {
      const matches: string[] = [];
      for (const node of tree_nodes) {
        let matches_search: boolean = true;
        if (search) {
          matches_search =
            node.name.toLowerCase().includes(search) ||
            node.db_name.toLowerCase().includes(search) ||
            node.path.toLowerCase().includes(search);
        }

        let matches_subscribed: boolean = true;
        if (show_only_subscribed) {
          const target_path: string =
            node.type === "db" ? node.db_name : `${node.db_name}/${node.path}`;
          matches_subscribed = subscribed_paths.has(target_path);
        }

        if (matches_search && matches_subscribed) {
          matches.push(node.id);
        }
      }

      for (const id of matches) {
        let curr_id: string | null = id;
        while (curr_id) {
          visible.add(curr_id);
          const node = tree_nodes.find((n) => n.id === curr_id);
          curr_id = node ? node.parent_row_id : null;
        }
      }
    } else {
      for (const node of tree_nodes) {
        let is_visible: boolean = true;
        let parent_id: string | null = node.parent_row_id;
        while (parent_id) {
          if (!expanded_nodes.has(parent_id)) {
            is_visible = false;
            break;
          }
          const parent_node = tree_nodes.find((n) => n.id === parent_id);
          parent_id = parent_node ? parent_node.parent_row_id : null;
        }
        if (is_visible) {
          visible.add(node.id);
        }
      }
    }
    visible_node_ids = visible;
  }

  $: visible_nodes = tree_nodes.filter((n) => visible_node_ids.has(n.id));
  $: start_index = Math.max(0, Math.floor(scroll_top / row_height) - overscan);
  $: end_index = Math.min(
    visible_nodes.length,
    Math.ceil((scroll_top + viewport_height) / row_height) + overscan,
  );

  function expandAll() {
    for (const node of tree_nodes) {
      if (node.is_struct || node.is_array || node.type === "db") {
        expanded_nodes.add(node.id);
      }
    }
    expanded_nodes = new Set(expanded_nodes);
  }

  function collapseAll() {
    expanded_nodes.clear();
    expanded_nodes = new Set(expanded_nodes);
  }

  // Toggle expansion of process image tree node
  function toggleNode(nodeId: string) {
    if (expanded_nodes.has(nodeId)) {
      expanded_nodes.delete(nodeId);
    } else {
      expanded_nodes.add(nodeId);
    }
    expanded_nodes = new Set(expanded_nodes);
  }

  // ── Schema Registry subscription helpers ─────────────────────────────

  // Toggle subscription for a registry DB group
  function toggleRegDbSubscription(db_name: string, checked: boolean) {
    const target_path = db_name;
    if (checked) {
      subscribed_paths.add(target_path);
      sendWorkerCommand("subscribe", { path: target_path });
    } else {
      subscribed_paths.delete(target_path);
      sendWorkerCommand("unsubscribe", { path: target_path });
      // Also remove any field-level subscriptions under this DB
      for (const p of subscribed_paths) {
        if (p.startsWith(db_name + "/")) {
          subscribed_paths.delete(p);
          sendWorkerCommand("unsubscribe", { path: p });
        }
      }
    }
    subscribed_paths = new Set(subscribed_paths);
  }

  // Toggle subscription for a registry field
  function toggleRegFieldSubscription(
    t_db_name: string,
    t_field_path: string,
    t_checked: boolean,
  ) {
    const target_path: string = `${t_db_name}/${t_field_path}`;
    if (t_checked) {
      subscribed_paths.add(target_path);
      sendWorkerCommand("subscribe", { path: target_path });
    } else {
      subscribed_paths.delete(target_path);
      sendWorkerCommand("unsubscribe", { path: target_path });
    }
    subscribed_paths = new Set(subscribed_paths);
  }

  // Check if a registry DB is fully subscribed (all fields covered)
  function isRegDbFullySubscribed(db_name: string): boolean {
    return subscribed_paths.has(db_name);
  }

  // Check if a registry field is subscribed
  function isRegFieldSubscribed(
    t_db_name: string,
    t_field_path: string,
  ): boolean {
    return subscribed_paths.has(`${t_db_name}/${t_field_path}`);
  }

  // Toggle expansion of registry group headers
  function toggleRegGroup(t_group_id: string) {
    if (expanded_reg_groups.has(t_group_id)) {
      expanded_reg_groups.delete(t_group_id);
    } else {
      expanded_reg_groups.add(t_group_id);
    }
    expanded_reg_groups = new Set(expanded_reg_groups);
  }

  // Filter Data Blocks for Schema Registry search
  // Filter UDTs for Schema Registry search
  // Filter Tags for Schema Registry search
</script>

<div class="dashboard-wrap">
  <!-- Tab bar -->
  <div class="tab-bar">
    <button
      class="tab-btn"
      class:active={active_tab === "process"}
      on:click={() => (active_tab = "process")}
    >
      Process Image
    </button>
    <button
      class="tab-btn"
      class:active={active_tab === "registry"}
      on:click={() => (active_tab = "registry")}
    >
      Schema Registry
    </button>
    {#if active_tab === "process"}
      <div class="tab-right">
        <label class="dash-sub-label">
          <input
            type="checkbox"
            bind:checked={show_only_subscribed}
            class="dash-check"
          />
          Subscribed only
        </label>
        <button
          class="dash-btn mr-8"
          on:click={() => {
            if (stream_paused) {
              stream_paused = false;
              for (const path of paused_paths) {
                subscribed_paths.add(path);
                sendWorkerCommand("subscribe", { path });
              }
              subscribed_paths = new Set(subscribed_paths);
              paused_paths.clear();
              cell_map.clear();
              update_count = 0;
            } else {
              paused_paths = new Set(subscribed_paths);
              stream_paused = true;
              sendWorkerCommand("clear_subscriptions");
              subscribed_paths.clear();
              subscribed_paths = new Set(subscribed_paths);
            }
          }}
        >
          {stream_paused ? "▶ Resume" : "⏸ Pause"}
        </button>
        <input
          class="search-box"
          placeholder="Filter by block or path…"
          bind:value={search_query}
        />
        <span class="update-count">Updates: {update_count}</span>
      </div>
    {:else if active_tab === "registry"}
      <div class="tab-right">
        <input
          class="search-box"
          placeholder="Filter schema (UDT, DB, Tag)…"
          bind:value={registry_search_query}
        />
      </div>
    {/if}
  </div>

  <!-- Process Image tab -->
  {#if active_tab === "process"}
    <div
      class="table-area"
      on:scroll={(e) => (scroll_top = e.currentTarget.scrollTop)}
      bind:clientHeight={viewport_height}
    >
      {#if loading_registry}
        <div class="state-center">
          <div class="spinner"></div>
          <span>Initializing Registry…</span>
        </div>
      {:else if registry_error}
        <div class="state-center error">⚠️ {registry_error}</div>
      {:else if !tree_nodes.length}
        <div class="state-center">No databases in registry response.</div>
      {:else}
        <table>
          <thead>
            <tr>
              <th class="th-block">Block</th>
              <th>
                Hierarchical Path
                <button class="dash-btn-alt ml-8" on:click={expandAll}
                  >Expand All</button
                >
                <button class="dash-btn-alt ml-4" on:click={collapseAll}
                  >Collapse All</button
                >
              </th>
              <th class="th-live">Live Value</th>
              <th class="th-unit">Unit</th>
              <th class="th-range">Range</th>
              <th class="th-sync">Sync Time</th>
            </tr>
          </thead>
          <tbody>
            {#if start_index > 0}
              <tr style="height: {start_index * row_height}px;"></tr>
            {/if}
            {#each visible_nodes.slice(start_index, end_index) as node (node.id)}
              {@const cell_entry = cell_map.get(node.key)}
              <tr
                class={node.type === "db" ? "db-root-row" : "child-row"}
                style="height: {row_height}px;"
              >
                <!-- BLOCK BADGE -->
                <td>
                  {#if node.type === "db"}
                    <span class="db-badge">DB{node.db_num}</span>
                  {/if}
                </td>

                <!-- PATH & TOGGLE -->
                <td>
                  <div
                    class="tree-cell"
                    style="padding-left:{node.depth * 28}px"
                  >
                    {#each Array(Math.max(0, node.depth - 1)) as _}
                      <span class="depth-marker"></span>
                    {/each}

                    {#if node.is_struct || node.is_array}
                      <span
                        class="toggle-icon"
                        class:collapsed={!expanded_nodes.has(node.id)}
                        on:click={() => toggleNode(node.id)}>▼</span
                      >
                    {:else}
                      <span class="indent-gap"></span>
                    {/if}

                    <span
                      class={node.type === "db"
                        ? "db-title"
                        : node.is_struct || node.is_array
                          ? "struct-node path-segment"
                          : "leaf-node path-segment"}
                      style="cursor:{node.is_struct || node.is_array
                        ? 'pointer'
                        : 'default'}"
                      on:click={() =>
                        (node.is_struct || node.is_array) &&
                        toggleNode(node.id)}
                    >
                      {node.name}
                    </span>
                  </div>
                </td>

                <!-- LIVE VALUE -->
                <td class="value-cell" class:has-value={!!cell_entry}>
                  {#if node.type === "db" || node.is_struct || node.is_array}
                    <span class="ghost-text">—</span>
                  {:else if node.field_type === "TRUNCATED"}
                    <span class="ghost-italic">—</span>
                  {:else}
                    {cell_entry?.val ?? "--"}
                  {/if}
                </td>

                <!-- UNIT -->
                <td class="unit-cell">
                  {#if node.type === "db" || node.is_struct || node.is_array || node.field_type === "TRUNCATED"}
                    —
                  {:else}
                    {node.unit ?? "—"}
                  {/if}
                </td>

                <!-- RANGE -->
                <td class="range-cell">
                  {#if node.type === "db" || node.is_struct || node.is_array || node.field_type === "TRUNCATED"}
                    —
                  {:else if node.min !== undefined && node.max !== undefined}
                    [{node.min}, {node.max}]
                  {:else}
                    —
                  {/if}
                </td>

                <!-- SYNC TIME -->
                <td class="sync-cell">
                  {#if node.type === "db" || node.is_struct || node.is_array || node.field_type === "TRUNCATED"}
                    —
                  {:else}
                    {cell_entry?.sync ?? "--"}
                  {/if}
                </td>
              </tr>
            {/each}
            {#if visible_nodes.length > end_index}
              <tr
                style="height: {(visible_nodes.length - end_index) *
                  row_height}px;"
              ></tr>
            {/if}
          </tbody>
        </table>
      {/if}
    </div>

    <!-- Schema Registry tab -->
  {:else if active_tab === "registry"}
    <div class="table-area">
      {#if loading_registry}
        <div class="state-center">
          <div class="spinner"></div>
          <span>Loading Static Schema…</span>
        </div>
      {:else if registry_error}
        <div class="state-center error">{registry_error}</div>
      {:else}
        <table>
          <thead>
            <tr>
              <th class="th-sub">Sub</th>
              <th class="th-offset">Offset</th>
              <th class="th-type">Type</th>
              <th>Name / Path</th>
              <th>Unit</th>
              <th class="th-range">Range / Enum</th>
              <th class="th-count">Count</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            <!-- 1. DATA BLOCKS -->
            {#if registry?.dbs}
              {#each filterDbs(registry.dbs, registry_search_query.toLowerCase()) as db, idx}
                {@const group_id = `reg-group-db-${idx}`}
                <tr
                  class="reg-group-header"
                  on:click={() => toggleRegGroup(group_id)}
                >
                  <td class="cell-center" on:click|stopPropagation>
                    <input
                      type="checkbox"
                      checked={isRegDbFullySubscribed(db.name)}
                      on:change={(e) =>
                        toggleRegDbSubscription(
                          db.name,
                          e.currentTarget.checked,
                        )}
                      class="dash-check"
                    />
                  </td>
                  <td colspan="7">
                    <div class="reg-group-title">
                      <span
                        class="reg-group-chevron"
                        class:collapsed={!expanded_reg_groups.has(group_id)}
                        >▼</span
                      >
                       <span class="db-badge mr-6">DB{db.number}</span>
                      <strong>{db.name}</strong>
                      <span class="meta-muted">{db.size_bytes} B</span>
                      {#if db.endianness}
                        <span class="btn-sm active ml-8"
                          >{db.endianness}-ENDIAN</span
                        >
                      {/if}
                      {#if db.trigger_events}
                        <span class="btn-sm active warn ml-4">EVENTS</span>
                      {/if}
                      {#if isRegDbFullySubscribed(db.name)}
                        <span class="sub-badge sub-badge-active"
                          >SUBSCRIBED</span
                        >
                      {/if}
                    </div>
                  </td>
                </tr>

                {#if expanded_reg_groups.has(group_id)}
                  <tr class="th-row">
                    <td class="col-gap"></td>
                    <td class="th-upper">Offset</td>
                    <td class="th-upper">Type</td>
                    <td class="th-upper">Name / Path</td>
                    <td class="th-upper">Unit</td>
                    <td class="th-upper">Range / Enum</td>
                    <td class="th-upper-right">Count</td>
                    <td></td>
                  </tr>

                  {#if db.fields && db.fields.length > 0}
                    {#each flattenRegistryFields(db.fields, registry) as f}
                      {#if !registry_search_query || f.name
                          .toLowerCase()
                          .includes(registry_search_query.toLowerCase())}
                        <tr class="reg-row">
                          <td class="cell-center">
                            <input
                              type="checkbox"
                              checked={isRegFieldSubscribed(
                                db.name,
                                f.full_path,
                              )}
                              on:change={(e) =>
                                toggleRegFieldSubscription(
                                  db.name,
                                  f.full_path,
                                  e.currentTarget.checked,
                                )}
                              class="dash-check"
                            />
                          </td>
                          <td class="mono-muted">{f.offset}</td>
                          <td
                            ><span
                              class="type-badge"
                              class:udt-badge={!!f.udt_name}
                              >{f.udt_name || f.type}</span
                            ></td
                          >
                          <td>
                            <div
                              class="tree-cell"
                              style="padding-left:{f.depth * 14}px; gap:4px"
                            >
                              {#if f.depth > 0}
                                <span class="field-mark">└─</span>
                              {/if}
                              <span
                                class={f.udt_name ? "struct-node" : "leaf-node"}
                                >{f.name}</span
                              >
                            </div>
                          </td>
                          <td class="unit-cell">{f.unit ?? "—"}</td>
                          <td class="range-cell">
                            {#if f.min !== undefined && f.max !== undefined}
                              [{f.min}, {f.max}]
                            {/if}
                            {#if f.enum_map}
                              <span class="meta-muted"
                                >{Object.entries(f.enum_map)
                                  .map(([k, v]) => `${k}=${v}`)
                                  .join(", ")}</span
                              >
                            {/if}
                            {#if f.min === undefined && !f.enum_map}
                              —
                            {/if}
                          </td>
                          <td class="cell-right-dim"
                            >{f.count > 1 ? f.count : ""}</td
                          >
                          <td>
                            {#if isRegFieldSubscribed(db.name, f.full_path)}
                              <span class="sub-dot"></span>
                            {/if}
                          </td>
                        </tr>
                      {/if}
                    {/each}
                  {:else}
                    <tr class="reg-row">
                      <td colspan="8" class="empty-cell">No fields mapped</td>
                    </tr>
                  {/if}
                {/if}
              {/each}
            {/if}

            <!-- 2. UDTS -->
            {#if registry?.udts}
              {#each filterUdts(registry.udts, registry_search_query.toLowerCase()) as udt, idx}
                {@const group_id = `reg-group-udt-${idx}`}
                <tr
                  class="reg-group-header"
                  on:click={() => toggleRegGroup(group_id)}
                >
                  <td colspan="8">
                    <div class="reg-group-title">
                      <span
                        class="reg-group-chevron"
                        class:collapsed={!expanded_reg_groups.has(group_id)}
                        >▼</span
                      >
                      <span class="type-badge udt-badge mr-6">UDT</span>
                      <strong>{udt.name}</strong>
                      <span class="meta-muted">{udt.size_bytes} B</span>
                    </div>
                  </td>
                </tr>

                {#if expanded_reg_groups.has(group_id)}
                  <tr class="th-row">
                    <td class="col-gap"></td>
                    <td class="th-upper">Offset</td>
                    <td class="th-upper">Type</td>
                    <td class="th-upper">Member</td>
                    <td class="th-upper">Unit</td>
                    <td class="th-upper">Range / Enum</td>
                    <td class="th-upper-right">Count</td>
                    <td></td>
                  </tr>

                  {#if udt.fields && udt.fields.length > 0}
                    {#each flattenRegistryFields(udt.fields, registry) as f}
                      {#if !registry_search_query || f.name
                          .toLowerCase()
                          .includes(registry_search_query.toLowerCase())}
                        <tr class="reg-row">
                          <td class="col-gap"></td>
                          <td class="mono-muted">{f.offset}</td>
                          <td
                            ><span
                              class="type-badge"
                              class:udt-badge={!!f.udt_name}
                              >{f.udt_name || f.type}</span
                            ></td
                          >
                          <td>
                            <div
                              class="tree-cell"
                              style="padding-left:{f.depth * 14}px; gap:4px"
                            >
                              {#if f.depth > 0}
                                <span class="field-mark">└─</span>
                              {/if}
                              <span
                                class={f.udt_name ? "struct-node" : "leaf-node"}
                                >{f.name}</span
                              >
                            </div>
                          </td>
                          <td class="unit-cell">{f.unit ?? "—"}</td>
                          <td class="range-cell">
                            {#if f.min !== undefined && f.max !== undefined}
                              [{f.min}, {f.max}]
                            {/if}
                            {#if f.enum_map}
                              <span class="meta-muted"
                                >{Object.entries(f.enum_map)
                                  .map(([k, v]) => `${k}=${v}`)
                                  .join(", ")}</span
                              >
                            {/if}
                            {#if f.min === undefined && !f.enum_map}
                              —
                            {/if}
                          </td>
                          <td class="cell-right-dim"
                            >{f.count > 1 ? f.count : ""}</td
                          >
                          <td></td>
                        </tr>
                      {/if}
                    {/each}
                  {:else}
                    <tr class="reg-row">
                      <td colspan="8" class="empty-cell">No members</td>
                    </tr>
                  {/if}
                {/if}
              {/each}
            {/if}

            <!-- 3. TAGS -->
            {#if registry?.tags && filterTags(registry.tags, registry_search_query.toLowerCase()).length > 0}
              {@const group_id = "reg-group-tags"}
              <tr
                class="reg-group-header"
                on:click={() => toggleRegGroup(group_id)}
              >
                <td colspan="8">
                  <div class="reg-group-title">
                    <span
                      class="reg-group-chevron"
                      class:collapsed={!expanded_reg_groups.has(group_id)}
                      >▼</span
                    >
                    <span class="mr-6 text-dim">◈</span>
                    <strong>PLC Global Symbol Tags</strong>
                    <span class="meta-muted">
                      {filterTags(
                        registry.tags,
                        registry_search_query.toLowerCase(),
                      ).length} entries
                    </span>
                  </div>
                </td>
              </tr>

              {#if expanded_reg_groups.has(group_id)}
                <tr class="th-row">
                  <td class="col-gap"></td>
                  <td class="th-upper">Address</td>
                  <td class="th-upper">Type</td>
                  <td class="th-upper">Symbol Name</td>
                  <td class="th-upper">Unit</td>
                  <td class="th-upper">Range</td>
                  <td colspan="2" class="th-upper">Remark</td>
                </tr>

                {#each filterTags(registry.tags, registry_search_query.toLowerCase()) as t, idx}
                  <tr class="reg-row">
                    <td class="col-gap"></td>
                    <td class="accent-mono">{t.address}</td>
                    <td><span class="type-badge">{t.type}</span></td>
                    <td class="text-semibold">{t.name}</td>
                    <td class="unit-cell">—</td>
                    <td class="range-cell">—</td>
                    <td colspan="2" class="cell-dim-sm">{t.remark ?? "--"}</td>
                  </tr>
                {/each}
              {/if}
            {/if}
          </tbody>
        </table>
      {/if}
    </div>
  {/if}
</div>
