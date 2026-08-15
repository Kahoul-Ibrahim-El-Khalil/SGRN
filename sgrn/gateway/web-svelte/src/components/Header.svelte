<script lang="ts">
  import { link, location } from "svelte-spa-router";
  import { theme } from "../lib/theme";
  import {
    packets,
    rate,
    statusText,
    statusDotClass,
  } from "../lib/telemetryStore";

  function toggleTheme() {
    theme.update((t) => (t === "light" ? "dark" : "light"));
  }
</script>

<header>
  <div class="brand">
    <div class="logo-orb"></div>
    <h1>
      SGRN <span style="font-weight:400; opacity:.55; letter-spacing:.5px"
        >S7 Gateway</span
      >
    </h1>
  </div>
  <nav class="header-nav">
    <a
      href="/"
      use:link
      class="nav-link"
      class:active={$location === "/" || $location.startsWith("/dashboard")}
      >Dashboard</a
    >
    <a
      href="/projections"
      use:link
      class="nav-link"
      class:active={$location === "/projections"}>Projections</a
    >
    <a
      href="/policy"
      use:link
      class="nav-link"
      class:active={$location === "/policy"}>Policy</a
    >
    <a
      href="/docs"
      use:link
      class="nav-link"
      class:active={$location.startsWith("/docs")}>Docs</a
    >
  </nav>
  <div class="header-controls">
    <div class="stat-group">
      <div class="stat-item">
        <span class="stat-label">Packets</span>
        <span class="stat-value">{$packets}</span>
      </div>
      <div class="toolbar-sep"></div>
      <div class="stat-item">
        <span class="stat-label">Rate</span>
        <span class="stat-value">{$rate}&thinsp;Hz</span>
      </div>
      <div class="toolbar-sep"></div>
      <div class="stat-item">
        <span
          class="status-dot {$statusDotClass}"
          title="Telemetry connection status"
        ></span>
        <span class="stat-value">{$statusText}</span>
      </div>
    </div>
    <div class="toolbar-sep"></div>
    <button class="theme-toggle" on:click={toggleTheme} title="Toggle theme">
      {$theme === "dark" ? "☀" : "🌙"}
    </button>
  </div>
</header>

<style>
  header {
    padding: 0 8px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    background: var(--toolbar-bg);
    border-bottom: 1px solid var(--toolbar-border);
    flex-shrink: 0;
    z-index: 100;
    gap: 12px;
  }

  .brand {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-shrink: 0;
  }

  .logo-orb {
    width: 8px;
    height: 8px;
    background: var(--accent);
    border-radius: 1px;
    flex-shrink: 0;
  }

  .brand h1 {
    font-size: 11px;
    margin: 0;
    letter-spacing: 1.5px;
    font-weight: 700;
    color: var(--text);
    text-transform: uppercase;
    white-space: nowrap;
  }

  .header-nav {
    display: flex;
    align-items: center;
    gap: 4px;
  }

  .nav-link {
    font-size: 11px;
    padding: 2px 8px;
    border-radius: 2px;
    color: var(--text-muted);
    font-weight: 600;
  }

  .nav-link:hover {
    background: var(--accent-light);
    color: var(--accent);
  }

  .nav-link.active {
    background: var(--accent);
    color: #ffffff;
  }

  .header-controls {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .toolbar-sep {
    width: 1px;
    height: 16px;
    background: var(--toolbar-border);
    flex-shrink: 0;
  }

  .stat-group {
    display: flex;
    align-items: center;
    gap: 16px;
  }

  .stat-item {
    display: flex;
    align-items: center;
    gap: 5px;
  }

  .stat-label {
    font-size: 10px;
    color: var(--text-muted);
    text-transform: uppercase;
    letter-spacing: 0.8px;
    font-weight: 600;
  }

  .stat-value {
    font-family: var(--mono);
    font-size: 11px;
    color: var(--accent);
    font-weight: 700;
  }

  .status-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    display: inline-block;
  }

  .status-dot.offline {
    background-color: var(--text-muted);
  }

  .status-dot.connected {
    background-color: var(--success);
  }

  .status-dot.error {
    background-color: var(--error);
  }

  .theme-toggle {
    background: none;
    border: none;
    cursor: pointer;
    font-size: 14px;
    color: var(--text);
    padding: 0 4px;
    display: flex;
    align-items: center;
    justify-content: center;
  }
</style>
