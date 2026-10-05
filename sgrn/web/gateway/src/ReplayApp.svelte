<script lang="ts">
  import { onMount } from "svelte";
  import Router from "svelte-spa-router";
  import Dashboard from "./pages/Dashboard.svelte";
  import Header from "./components/Header.svelte";
  import { theme } from "./lib/theme";
  import { bootTelemetry } from "./lib/bootTelemetry";
  theme.subscribe(() => {});

  // Replay dashboard: inherits the shared shell (Header, Dashboard,
  // telemetry boot, ReplayControl inside Dashboard) but is NOT the gateway
  // dashboard — no Projections/Policy/Docs routes, so the documentation
  // bundle (@docs/*.md via DocsContent) is never loaded here.
  const routes = {
    "/": Dashboard,
    "*": Dashboard,
  };

  onMount(() => {
    bootTelemetry();
  });
</script>

<div class="app-container">
  <Header variant="replay" />
  <div class="main-body">
    <Router {routes} />
  </div>
</div>
