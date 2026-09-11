<script lang="ts">
  export let ACTION: "ALLOW" | "DENY" | string;
</script>

<span class="action-badge {ACTION}">
  {ACTION === "ALLOW" ? "✓" : "✕"}
  {ACTION}
</span>

<style>
  .action-badge {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 2px 8px;
    border-radius: 3px;
    font-size: 10px;
    font-weight: 700;
    letter-spacing: 0.5px;
  }
  .action-badge.ALLOW {
    background: #e6f4ea;
    color: var(--success);
    border: 1px solid #b7dfbe;
  }
  .action-badge.DENY {
    background: #fce8e8;
    color: var(--error);
    border: 1px solid #f5b5b5;
  }

  :global([data-theme="dark"]) .action-badge.ALLOW {
    background: rgba(46, 125, 50, 0.2);
    border-color: rgba(46, 125, 50, 0.4);
  }
  :global([data-theme="dark"]) .action-badge.DENY {
    background: rgba(198, 40, 40, 0.2);
    border-color: rgba(198, 40, 40, 0.4);
  }
</style>
