<script lang="ts">
  import { onMount } from "svelte";
  import {
    matchSecurityRule,
    fetchSecurityPolicy,
    type SecurityRule,
  } from "@sgrn/gateway";
  import ProtoPill from "../components/ProtoPill.svelte";
  import ActionBadge from "../components/ActionBadge.svelte";
  import Tag from "../components/Tag.svelte";

  let allRules: SecurityRule[] = [];
  let loading = true;
  let error = "";
  let lastUpdated = "";

  // Filters
  let fProto = "";
  let fAction = "";
  let fIp = "";
  let fDb = "";

  // IP Test Tool
  let testIp = "";
  let testProto = "S7";
  let testDb = "";
  let testResult = "";
  let testResultClass = "";

  onMount(async () => {
    await loadPolicy();
  });

  async function loadPolicy() {
    loading = true;
    error = "";
    try {
      const data = await fetchSecurityPolicy();
      allRules = data.rules || [];
      lastUpdated = new Date().toLocaleTimeString();
    } catch (e: any) {
      error = e.message;
    } finally {
      loading = false;
    }
  }

  // Specificity math for visualization
  $: maxSpec = allRules.reduce((m, r) => Math.max(m, r.specificity), 0);

  // Filtered rules
  $: filteredRules = allRules.filter(r => {
    if (fProto && r.protocol !== fProto) return false;
    if (fAction && r.action !== fAction) return false;
    if (fIp) {
      const cidrText = (r.cidrs || []).join(' ').toLowerCase();
      if (!cidrText.includes(fIp.toLowerCase())) return false;
    }
    if (fDb !== "") {
      const dbNum = parseInt(fDb);
      if (!r.any_db && !r.dbs.includes(dbNum)) return false;
    }
    return true;
  });

  // Count stats
  $: totalRules = allRules.length;
  $: allowCount = allRules.filter(r => r.action === 'ALLOW').length;
  $: denyCount = allRules.filter(r => r.action === 'DENY').length;
  $: s7Count = allRules.filter(r => r.protocol === 'S7').length;
  $: httpCount = allRules.filter(r => r.protocol === 'HTTP').length;
  $: wsCount = allRules.filter(r => r.protocol === 'WebSocket').length;
  $: modbusCount = allRules.filter(r => r.protocol === 'Modbus').length;
  $: opcuaCount = allRules.filter(r => r.protocol === 'OPC-UA').length;
  $: eipCount = allRules.filter(r => r.protocol === 'EtherNet/IP').length;

  function simulateAccess() {
    if (!testIp) {
      testResult = "";
      testResultClass = "";
      return;
    }
    const db = testDb !== "" ? parseInt(testDb) : null;
    for (const r of allRules) {
      const verdict = matchSecurityRule(r, testIp, testProto, db);
      if (verdict !== null) {
        testResult = verdict === 'ALLOW' ? '✓ ALLOW' : '✕ DENY';
        testResultClass = verdict === 'ALLOW' ? 'allow' : 'deny';
        return;
      }
    }
    testResult = '✕ DENY (default)';
    testResultClass = 'deny';
  }

  // Detail Modal
  let selectedRule: SecurityRule | null = null;
</script>

<!-- Toolbar -->
<div class="policy-toolbar">
  <span class="toolbar-title">Active Security Policy</span>
  <span class="toolbar-sub">
    {#if loading}Loading…{:else}{totalRules} rule{totalRules !== 1 ? 's' : ''} loaded{/if}
  </span>
  <div class="spacer"></div>
  {#if lastUpdated}<span class="ts-badge">Last updated: {lastUpdated}</span>{/if}
  <button class="btn" on:click={loadPolicy}>⟳ Refresh</button>
</div>

<!-- Stats bar -->
<div class="stats-bar">
  <div class="stat-chip"><span class="val">{totalRules}</span><span class="lbl">Total Rules</span></div>
  <div class="stat-chip"><span class="val">{allowCount}</span><span class="lbl">Allow</span></div>
  <div class="stat-chip"><span class="val">{denyCount}</span><span class="lbl">Deny</span></div>
  <div class="stat-chip"><span class="val">{s7Count}</span><span class="lbl">S7</span></div>
  <div class="stat-chip"><span class="val">{httpCount}</span><span class="lbl">HTTP</span></div>
  <div class="stat-chip"><span class="val">{wsCount}</span><span class="lbl">WebSocket</span></div>
  <div class="stat-chip"><span class="val">{modbusCount}</span><span class="lbl">Modbus</span></div>
  <div class="stat-chip"><span class="val">{opcuaCount}</span><span class="lbl">OPC-UA</span></div>
  <div class="stat-chip"><span class="val">{eipCount}</span><span class="lbl">EtherNet/IP</span></div>
</div>

<!-- Filter bar -->
<div class="filter-bar">
  <label for="filter-proto">Protocol</label>
  <select id="filter-proto" bind:value={fProto}>
    <option value="">All</option>
    <option value="S7">S7</option>
    <option value="HTTP">HTTP</option>
    <option value="WebSocket">WebSocket</option>
    <option value="Modbus">Modbus</option>
    <option value="OPC-UA">OPC-UA</option>
    <option value="EtherNet/IP">EtherNet/IP</option>
  </select>

  <label for="filter-action">Action</label>
  <select id="filter-action" bind:value={fAction}>
    <option value="">All</option>
    <option value="ALLOW">Allow</option>
    <option value="DENY">Deny</option>
  </select>

  <label for="filter-ip">IP / CIDR</label>
  <input id="filter-ip" type="text" placeholder="e.g. 10.0.1" bind:value={fIp} style="width:130px">

  <label for="filter-db">DB #</label>
  <input id="filter-db" type="number" placeholder="e.g. 11" bind:value={fDb} style="width:70px">

  <div class="spacer"></div>
  <span id="filter-count" style="font-size:11px;color:var(--text-muted)">
    {filteredRules.length === totalRules ? `${totalRules} rules` : `${filteredRules.length} / ${totalRules} rules`}
  </span>
</div>

<!-- Content area -->
<div class="policy-content">
  {#if loading}
    <div class="state-block">
      <div class="spinner"></div>
      <div class="state-msg">Loading policy…</div>
    </div>
  {:else if error}
    <div class="state-block">
      <div class="state-icon">⚠️</div>
      <div class="state-msg">Failed to load policy</div>
      <div class="state-sub">{error} — is the gateway running?</div>
    </div>
  {:else if filteredRules.length === 0}
    <div class="state-block">
      <div class="state-icon">🛡️</div>
      <div class="state-msg">No rules match your filters</div>
      <div class="state-sub">Adjust the filters above.</div>
    </div>
  {:else}
    <div class="rule-table-wrap">
      <table class="rule-table">
        <thead>
          <tr>
            <th>#</th>
            <th>Protocol</th>
            <th>Action</th>
            <th>IP / CIDR</th>
            <th>DB #</th>
            <th>Origins</th>
            <th>Req. Headers</th>
            <th>OPC-UA Sessions</th>
            <th>Specificity</th>
          </tr>
        </thead>
        <tbody>
          {#each filteredRules as r, idx}
            <tr class="rule-row" on:click={() => selectedRule = r}>
              <td><span style="font-family:var(--mono);color:var(--text-muted);font-size:10px">{idx + 1}</span></td>
              <td><ProtoPill proto={r.protocol} /></td>
              <td><ActionBadge action={r.action} /></td>
              <td>
                <div class="tag-list">
                  {#if r.cidrs && r.cidrs.length}
                    {#each r.cidrs as c}
                      <Tag value={c} type="cidr" />
                    {/each}
                  {:else}
                    <Tag value="any" type="any" />
                  {/if}
                </div>
              </td>
              <td>
                <div class="tag-list">
                  {#if r.any_db}
                    <Tag value="any" type="any" />
                  {:else}
                    {#each r.dbs as d}
                      <Tag value={String(d)} type="db" />
                    {/each}
                  {/if}
                </div>
              </td>
              <td>
                <div class="tag-list">
                  {#if r.origins && r.origins.length}
                    {#each r.origins as o}
                      <Tag value={o} type="origin" />
                    {/each}
                  {:else}
                    <Tag value="any" type="any" />
                  {/if}
                </div>
              </td>
              <td>
                <div class="tag-list">
                  {#if r.headers && r.headers.length}
                    {#each r.headers as h}
                      <Tag value={h} type="header" />
                    {/each}
                  {:else}
                    <Tag value="none" type="any" />
                  {/if}
                </div>
              </td>
              <td>
                <div class="tag-list">
                  {#if r.sessions && r.sessions.length}
                    {#each r.sessions as s}
                      <Tag value={s} type="session" />
                    {/each}
                  {:else}
                    <Tag value="any" type="any" />
                  {/if}
                </div>
              </td>
              <td>
                <div class="spec-bar">
                  <div class="spec-track">
                    <div class="spec-fill" style="width:{maxSpec > 0 ? Math.min(100, Math.round(r.specificity / maxSpec * 100)) : 0}%"></div>
                  </div>
                  <span class="spec-num">{r.specificity}</span>
                </div>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}
</div>

<!-- IP test tool -->
<div class="test-tool">
  <label for="test-ip">Test IP:</label>
  <input id="test-ip" type="text" placeholder="e.g. 10.0.1.5" bind:value={testIp} on:keydown={e => e.key === 'Enter' && simulateAccess()} spellcheck="false">
  <label for="test-proto">Protocol:</label>
  <select id="test-proto" bind:value={testProto}>
    <option value="S7">S7</option>
    <option value="HTTP">HTTP</option>
    <option value="WebSocket">WebSocket</option>
    <option value="Modbus">Modbus</option>
    <option value="OPC-UA">OPC-UA</option>
    <option value="EtherNet/IP">EtherNet/IP</option>
  </select>
  <label for="test-db">DB:</label>
  <input id="test-db" type="number" placeholder="any" bind:value={testDb} style="width:60px">
  <button class="btn primary" on:click={simulateAccess}>Test</button>
  {#if testResult}
    <span class="test-result {testResultClass}">{testResult}</span>
  {/if}
  <div class="spacer"></div>
  <span style="font-size:10px;color:var(--text-muted)">Client-side rule simulation (same logic as gateway)</span>
</div>

<!-- Detail modal -->
{#if selectedRule}
  <div class="modal-overlay open" on:click={e => e.target === e.currentTarget && (selectedRule = null)}>
    <div class="modal-box">
      <button class="modal-close" on:click={() => selectedRule = null}>✕</button>
      <div class="modal-title">Rule — {selectedRule.protocol} {selectedRule.action}</div>
      <div id="modal-body">
        <div class="detail-section">
          <div class="detail-section-title">Protocol & Action</div>
          <div style="display:flex;gap:8px;flex-wrap:wrap">
            <ProtoPill proto={selectedRule.protocol} />
            <ActionBadge action={selectedRule.action} />
            <span style="font-size:10px;color:var(--text-muted);align-self:center">specificity: {selectedRule.specificity}</span>
          </div>
        </div>
        <div class="detail-section">
          <div class="detail-section-title">IP / CIDR predicates</div>
          <div class="tag-list">
            {#if selectedRule.cidrs && selectedRule.cidrs.length}
              {#each selectedRule.cidrs as c}
                <Tag value={c} type="cidr" />
              {/each}
            {:else}
              <Tag value="any" type="any" />
            {/if}
          </div>
        </div>
        <div class="detail-section">
          <div class="detail-section-title">Data Block #</div>
          <div class="tag-list">
            {#if selectedRule.any_db}
              <Tag value="any DB" type="any" />
            {:else}
              {#each selectedRule.dbs as d}
                <Tag value={String(d)} type="db" />
              {/each}
            {/if}
          </div>
        </div>
        {#if selectedRule.origins && selectedRule.origins.length}
          <div class="detail-section">
            <div class="detail-section-title">Allowed Origins (HTTP/WS)</div>
            <div class="tag-list">
              {#each selectedRule.origins as o}
                <Tag value={o} type="origin" />
              {/each}
            </div>
          </div>
        {/if}
        {#if selectedRule.headers && selectedRule.headers.length}
          <div class="detail-section">
            <div class="detail-section-title">Required Headers</div>
            <div class="tag-list">
              {#each selectedRule.headers as h}
                <Tag value={h} type="header" />
              {/each}
            </div>
          </div>
        {/if}
        {#if selectedRule.sessions && selectedRule.sessions.length}
          <div class="detail-section">
            <div class="detail-section-title">OPC-UA Session Names</div>
            <div class="tag-list">
              {#each selectedRule.sessions as s}
                <Tag value={s} type="session" />
              {/each}
            </div>
          </div>
        {/if}
      </div>
    </div>
  </div>
{/if}

<style>
  .policy-toolbar {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 16px;
    background: var(--surface);
    border-bottom: 1px solid var(--surface-border);
    flex-shrink: 0;
    flex-wrap: wrap;
  }
  .policy-toolbar .toolbar-title {
    font-size: 13px;
    font-weight: 700;
    color: var(--text);
    margin-right: 4px;
  }
  .policy-toolbar .toolbar-sub {
    font-size: 11px;
    color: var(--text-muted);
  }
  .spacer { flex: 1; }

  .filter-bar {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 8px 16px;
    background: var(--bg);
    border-bottom: 1px solid var(--surface-border);
    flex-shrink: 0;
    flex-wrap: wrap;
  }
  .filter-bar label { font-size: 11px; color: var(--text-muted); font-weight: 600; }
  .filter-bar select, .filter-bar input {
    font-size: 11px;
    padding: 3px 8px;
    border: 1px solid var(--surface-border);
    border-radius: 3px;
    background: var(--input-bg);
    color: var(--text);
    outline: none;
  }
  .filter-bar select:focus, .filter-bar input:focus {
    border-color: var(--accent);
  }

  .stats-bar {
    display: flex;
    gap: 16px;
    padding: 8px 16px;
    background: var(--surface);
    border-bottom: 1px solid var(--surface-border);
    flex-shrink: 0;
    overflow-x: auto;
  }
  .stat-chip {
    display: flex;
    flex-direction: column;
    align-items: center;
    background: var(--surface-bright);
    border: 1px solid var(--surface-border);
    border-radius: 4px;
    padding: 6px 16px;
    min-width: 80px;
    flex-shrink: 0;
  }
  .stat-chip .val {
    font-size: 22px;
    font-weight: 700;
    color: var(--accent);
    line-height: 1.1;
  }
  .stat-chip .lbl {
    font-size: 9px;
    font-weight: 700;
    letter-spacing: .8px;
    text-transform: uppercase;
    color: var(--text-muted);
    margin-top: 2px;
  }

  .policy-content {
    flex: 1;
    overflow-y: auto;
    padding: 16px;
  }

  .rule-table-wrap {
    background: var(--surface-bright);
    border: 1px solid var(--surface-border);
    border-radius: 4px;
    overflow: hidden;
  }
  table.rule-table {
    width: 100%;
    border-collapse: collapse;
    font-size: 12px;
  }
  table.rule-table th {
    background: var(--th-bg);
    color: var(--th-text);
    font-weight: 700;
    font-size: 10px;
    letter-spacing: .6px;
    text-transform: uppercase;
    padding: 7px 12px;
    text-align: left;
    white-space: nowrap;
    border-bottom: 1px solid var(--surface-border);
  }
  table.rule-table tbody tr {
    transition: background .1s;
    cursor: pointer;
  }
  table.rule-table tbody tr:nth-child(even) { background: var(--row-alt); }
  table.rule-table tbody tr:hover { background: var(--row-hover); }
  table.rule-table td {
    padding: 8px 12px;
    border-bottom: 1px solid var(--surface-border);
    vertical-align: middle;
  }
  table.rule-table tbody tr:last-child td { border-bottom: none; }

  .spec-bar {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .spec-track {
    flex: 1;
    max-width: 72px;
    height: 4px;
    background: var(--surface-border);
    border-radius: 2px;
    overflow: hidden;
  }
  .spec-fill {
    height: 100%;
    background: var(--accent);
    border-radius: 2px;
    transition: width .3s ease;
  }
  .spec-num { font-size: 10px; color: var(--text-muted); font-family: var(--mono); }

  .detail-section { margin: 0 0 14px; }
  .detail-section-title {
    font-size: 9px; font-weight: 700; letter-spacing: 1px;
    text-transform: uppercase; color: var(--text-muted);
    margin-bottom: 6px;
  }

  .test-tool {
    display: flex;
    gap: 8px;
    align-items: center;
    padding: 10px 16px;
    background: var(--surface);
    border-top: 1px solid var(--surface-border);
    flex-shrink: 0;
  }
  .test-tool label { font-size: 11px; font-weight: 700; color: var(--text); white-space: nowrap; }
  .test-tool input {
    flex: 1; max-width: 180px;
    font-size: 11px; font-family: var(--mono);
    padding: 4px 8px;
    border: 1px solid var(--surface-border);
    border-radius: 3px;
    background: var(--input-bg); color: var(--text);
  }
  .test-tool select {
    font-size: 11px;
    padding: 4px 6px;
    border: 1px solid var(--surface-border);
    border-radius: 3px;
    background: var(--input-bg); color: var(--text);
  }
  .test-result {
    font-size: 11px; font-weight: 700; padding: 3px 10px;
    border-radius: 3px; display: inline;
  }
  .test-result.allow { background: #e6f4ea; color: var(--success); }
  .test-result.deny  { background: #fce8e8; color: var(--error);   }
  :global([data-theme="dark"]) .test-result.allow { background: rgba(46,125,50,.2); }
  :global([data-theme="dark"]) .test-result.deny { background: rgba(198,40,40,.2); }
</style>
