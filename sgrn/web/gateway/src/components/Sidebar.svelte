<script lang="ts">
  import { link, location } from "svelte-spa-router";
</script>

<nav class="docs-sidebar">
  <div class="sidebar-section-label">Overview</div>
  <a
    href="/docs"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs" || $location === "/docs/"}
    >Introduction</a
  >

  <div class="sidebar-section-label">Core Concepts</div>
  <a
    href="/docs/arch"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/arch"}>Architecture &amp; Data Flow</a
  >

  <div class="sidebar-section-label">Protocol Reference</div>
  <a
    href="/docs/rest"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/rest"}>REST / HTTP</a
  >
  <a
    href="/docs/websocket"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/websocket"}>WebSocket</a
  >
  <a
    href="/docs/opcua"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/opcua"}>OPC-UA</a
  >
  <a
    href="/docs/modbus"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/modbus"}>Modbus</a
  >
  <a
    href="/docs/eip"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/eip"}>EtherNet/IP</a
  >
  <a
    href="/docs/s7"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/s7"}>Raw S7</a
  >

  <div class="sidebar-section-label">Reference</div>
  <a
    href="/docs/config"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/config"}>Config Reference</a
  >
  <a
    href="/docs/persistence"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/persistence"}>Persistence</a
  >
  <a
    href="/docs/security"
    use:link
    class="sidebar-link"
    class:active={$location === "/docs/security"}>Security</a
  >
</nav>

<style>
  .docs-sidebar {
    width: 220px;
    flex-shrink: 0;
    background: var(--surface);
    border-right: 1px solid var(--surface-border);
    overflow-y: auto;
    padding: 12px 0 24px;
    height: 100%;
  }

  .sidebar-section-label {
    font-size: 9px;
    font-weight: 700;
    letter-spacing: 1px;
    text-transform: uppercase;
    color: var(--text-muted);
    padding: 12px 16px 4px;
  }

  .sidebar-link {
    display: block;
    font-size: 11px;
    padding: 5px 16px;
    color: var(--text-dim);
    font-weight: 600;
  }

  .sidebar-link:hover {
    background: var(--accent-light);
    color: var(--accent);
    text-decoration: none;
  }

  .sidebar-link.active {
    background: var(--accent-light);
    color: var(--accent);
    border-left: 3px solid var(--accent);
    padding-left: 13px;
  }
</style>
