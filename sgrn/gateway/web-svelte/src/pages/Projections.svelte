<script lang="ts">
  import { onMount } from "svelte";
  import {
    buildEipProjection,
    buildOpcuaProjection,
    fetchFullRegistry,
    fetchModbusMap,
    type EipProjectionRow,
    type ModbusMap,
    type RegistryResponse,
    type OpcuaProjectionRow,
  } from "@sgrn/gateway";

  let activeTab: "modbus" | "eip" | "opcua" = "modbus";
  let modbusSubTab: "holding" | "input" | "coils" | "discrete" = "holding";
  
  let registry: RegistryResponse | null = null;
  let modbusMap: ModbusMap | null = null;
  let loading = true;
  let error = "";

  onMount(async () => {
    try {
      registry = await fetchFullRegistry();
      try {
        modbusMap = await fetchModbusMap();
      } catch (e) {
        console.warn("Modbus map not available:", e);
        modbusMap = null;
      }
    } catch (e: any) {
      error = e.message;
    } finally {
      loading = false;
    }
  });

  let eipRows: EipProjectionRow[] = [];
  let opcuaRows: OpcuaProjectionRow[] = [];
  $: if (registry?.dbs) {
    eipRows = buildEipProjection(registry);
    opcuaRows = buildOpcuaProjection(registry);
  }
</script>

<div class="projections-container glass-panel">
  <header class="proj-header">
    <h2>Protocol Projections</h2>
    <p>Virtual memory maps and address spaces derived from the PLC schema.</p>
  </header>

  {#if loading}
    <div class="state-msg">Loading projections...</div>
  {:else if error}
    <div class="state-msg error">{error}</div>
  {:else}
    <div class="tabs">
      <button class:active={activeTab === 'modbus'} on:click={() => activeTab = 'modbus'}>Modbus TCP</button>
      <button class:active={activeTab === 'eip'} on:click={() => activeTab = 'eip'}>EtherNet/IP</button>
      <button class:active={activeTab === 'opcua'} on:click={() => activeTab = 'opcua'}>OPC-UA</button>
    </div>

    <div class="tab-content">
      {#if activeTab === 'modbus'}
        {#if !modbusMap}
          <div class="state-msg">
            Modbus is disabled in the gateway configuration or the mapping could not be generated.
          </div>
        {:else}
          <div class="modbus-subtabs">
            <button class:active={modbusSubTab === 'holding'} on:click={() => modbusSubTab = 'holding'}>Holding Registers</button>
            <button class:active={modbusSubTab === 'input'} on:click={() => modbusSubTab = 'input'}>Input Registers</button>
            <button class:active={modbusSubTab === 'coils'} on:click={() => modbusSubTab = 'coils'}>Coils</button>
            <button class:active={modbusSubTab === 'discrete'} on:click={() => modbusSubTab = 'discrete'}>Discrete Inputs</button>
          </div>

        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th>Address</th>
                {#if modbusSubTab === 'holding' || modbusSubTab === 'input'}
                  <th>Count</th>
                {/if}
                {#if modbusSubTab === 'coils' || modbusSubTab === 'discrete'}
                  <th>Bit Index</th>
                {/if}
                <th>Source (Schema Field)</th>
                <th>Type</th>
                <th>Access</th>
              </tr>
            </thead>
            <tbody>
              {#if modbusSubTab === 'holding' && modbusMap?.holding_registers}
                {#each modbusMap.holding_registers as reg}
                  <tr>
                    <td class="code">{reg.start}</td>
                    <td>{reg.count}</td>
                    <td class="source-col">{reg.source}</td>
                    <td class="code">{reg.type}</td>
                    <td>{reg.access.toUpperCase()}</td>
                  </tr>
                {/each}
              {:else if modbusSubTab === 'input' && modbusMap?.input_registers}
                {#each modbusMap.input_registers as reg}
                  <tr>
                    <td class="code">{reg.start}</td>
                    <td>{reg.count}</td>
                    <td class="source-col">{reg.source}</td>
                    <td class="code">{reg.type}</td>
                    <td>{reg.access.toUpperCase()}</td>
                  </tr>
                {/each}
              {:else if modbusSubTab === 'coils' && modbusMap?.coils}
                {#each modbusMap.coils as reg}
                  <tr>
                    <td class="code">{reg.address}</td>
                    <td>{reg.bit_index}</td>
                    <td class="source-col">{reg.source}</td>
                    <td class="code">{reg.type}</td>
                    <td>{reg.access.toUpperCase()}</td>
                  </tr>
                {/each}
              {:else if modbusSubTab === 'discrete' && modbusMap?.discrete_inputs}
                {#each modbusMap.discrete_inputs as reg}
                  <tr>
                    <td class="code">{reg.address}</td>
                    <td>{reg.bit_index}</td>
                    <td class="source-col">{reg.source}</td>
                    <td class="code">{reg.type}</td>
                    <td>{reg.access.toUpperCase()}</td>
                  </tr>
                {/each}
              {/if}
            </tbody>
          </table>
        </div>
        {/if}

      {:else if activeTab === 'eip'}
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th>DB Name</th>
                <th>CIP Instance</th>
                <th>CIP Attribute</th>
                <th>Schema Field</th>
                <th>CIP Type</th>
              </tr>
            </thead>
            <tbody>
              {#each eipRows as row}
                <tr>
                  <td><strong>{row.db}</strong></td>
                  <td class="code">{row.instance}</td>
                  <td class="code">{row.attr}</td>
                  <td class="source-col">{row.field}</td>
                  <td class="code type-cell">{row.cipType}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>

      {:else if activeTab === 'opcua'}
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th>DB Name</th>
                <th>OPC-UA NodeId</th>
                <th>Data Type</th>
                <th>Value Rank</th>
              </tr>
            </thead>
            <tbody>
              {#each opcuaRows as row}
                <tr>
                  <td><strong>{row.db}</strong></td>
                  <td class="source-col code">{row.nodeId}</td>
                  <td class="code type-cell">{row.dataType}</td>
                  <td>{row.valueRank}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </div>
  {/if}
</div>

<style>
  .projections-container {
    display: flex;
    flex-direction: column;
    height: 100%;
    padding: 20px;
    box-sizing: border-box;
    overflow: hidden;
  }

  .proj-header {
    margin-bottom: 20px;
  }

  .proj-header h2 {
    margin: 0 0 4px 0;
    font-size: 1.4rem;
    color: var(--text);
  }

  .proj-header p {
    margin: 0;
    color: var(--text-muted);
    font-size: 0.9rem;
  }

  .tabs {
    display: flex;
    gap: 8px;
    margin-bottom: 16px;
    border-bottom: 1px solid var(--border);
    padding-bottom: 8px;
  }

  .tabs button {
    background: transparent;
    border: none;
    padding: 8px 16px;
    color: var(--text-muted);
    font-size: 0.95rem;
    font-weight: 500;
    cursor: pointer;
    border-radius: 4px;
    transition: all 0.2s ease;
  }

  .tabs button:hover {
    color: var(--text);
    background: rgba(255, 255, 255, 0.05);
  }

  .tabs button.active {
    color: var(--accent);
    background: var(--accent-light);
  }

  .modbus-subtabs {
    display: flex;
    gap: 6px;
    margin-bottom: 12px;
  }

  .modbus-subtabs button {
    background: var(--panel-bg);
    border: 1px solid var(--border);
    color: var(--text);
    padding: 4px 12px;
    font-size: 0.8rem;
    border-radius: 12px;
    cursor: pointer;
    transition: all 0.2s;
  }

  .modbus-subtabs button.active {
    background: var(--accent);
    color: white;
    border-color: var(--accent);
  }

  .tab-content {
    flex: 1;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }

  .table-wrap {
    flex: 1;
    overflow-y: auto;
    border: 1px solid var(--border);
    border-radius: 6px;
    background: var(--panel-bg);
  }

  .data-table {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.85rem;
  }

  .data-table th {
    background: var(--toolbar-bg);
    color: var(--text-muted);
    font-weight: 600;
    text-align: left;
    padding: 10px 12px;
    position: sticky;
    top: 0;
    border-bottom: 1px solid var(--border);
    z-index: 1;
  }

  .data-table td {
    padding: 8px 12px;
    border-bottom: 1px solid var(--border-light);
    color: var(--text);
  }

  .data-table tr:hover td {
    background: rgba(255,255,255,0.03);
  }

  .code {
    font-family: 'JetBrains Mono', monospace;
  }

  .source-col {
    color: var(--accent);
    font-weight: 500;
  }

  .type-cell {
    color: #e5c07b;
  }

  .state-msg {
    padding: 20px;
    color: var(--text-muted);
    font-style: italic;
  }

  .error {
    color: #ff5555;
  }
</style>
