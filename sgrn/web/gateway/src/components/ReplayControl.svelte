<script lang="ts">
  // ReplayControl.svelte — runtime replay pacing for sgrn_replay gateways.
  // SIMULATED DATA ONLY. Talks to /replay/* on the same HTTP listener
  // (see HttpAdapter::registerRoutes). 404 = normal gateway, hide control.
  let speed: number = 10;
  let paused: boolean = false;
  let unpaced: boolean = false;
  let status_text: string = "replay: unknown";
  let available: boolean = true;

  async function refresh(): Promise<void> {
    try {
      const r = await fetch("/replay/status");
      if (r.status === 404) {
        available = false;
        return;
      }
      available = true;
      const j = await r.json();
      status_text = `speed=${j.speed}x frames=${j.frames} ts=${j.ts} ${j.paused ? "paused" : ""} ${j.unpaced ? "unpaced" : ""}`;
      if (typeof j.speed === "number") speed = j.speed;
      paused = !!j.paused;
      unpaced = !!j.unpacked || !!j.unpaced;
    } catch {
      // gateway not up yet; keep control visible, retry on interval
    }
  }

  async function applySpeed(v: number | string): Promise<void> {
    await fetch("/replay/speed", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ speed: v }),
    });
    await refresh();
  }

  async function togglePause(): Promise<void> {
    await fetch(paused ? "/replay/resume" : "/replay/pause", { method: "POST" });
    await refresh();
  }

  setInterval(refresh, 1000);
  refresh();
</script>

{#if available}
  <div class="replay-control">
    <label>
      Replay speed (sim-s / wall-s):
      <input
        type="range"
        min="0.1"
        max="200"
        step="0.1"
        bind:value={speed}
        on:change={() => applySpeed(speed)}
      />
      <input
        type="number"
        min="0.1"
        max="1e9"
        step="0.1"
        bind:value={speed}
        on:change={() => applySpeed(speed)}
      />
    </label>
    <button on:click={() => applySpeed("unpaced")}>Unpaced</button>
    <button on:click={togglePause}>{paused ? "Resume" : "Pause"}</button>
    <code>{status_text}</code>
  </div>
{/if}

<style>
  .replay-control {
    display: flex;
    gap: 0.5rem;
    align-items: center;
    flex-wrap: wrap;
    padding: 0.5rem 0;
  }
</style>
