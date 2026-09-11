<script lang="ts">
  export let value: string;
  export let type: string; // db, cidr, origin, header, session, any
</script>

<span class="tag {type}">
  {value}
</span>

<style>
  .tag {
    display: inline-flex;
    align-items: center;
    padding: 1px 6px;
    border-radius: 3px;
    font-size: 10px;
    font-family: var(--mono);
    border: 1px solid var(--surface-border);
    background: var(--surface);
    color: var(--text);
    white-space: nowrap;
  }
  .tag.cidr {
    border-color: #9ec6e0;
    background: #e8f4fb;
    color: #1a4a62;
  }
  .tag.db {
    border-color: #b0c8a8;
    background: #eaf3e6;
    color: #1e4a1a;
  }
  .tag.origin {
    border-color: #c8b0d4;
    background: #f3eaf8;
    color: #4a1a6a;
  }
  .tag.header {
    border-color: #d4c8a0;
    background: #f8f3e0;
    color: #6a4a00;
  }
  .tag.session {
    border-color: #d0a8b0;
    background: #f8eaed;
    color: #6a1a28;
  }
  .tag.any {
    border-color: var(--surface-border);
    color: var(--text-muted);
    font-style: italic;
  }

  :global([data-theme="dark"]) .tag.cidr {
    background: rgba(30, 100, 160, 0.2);
    border-color: rgba(30, 100, 160, 0.5);
    color: #7ac0e8;
  }
  :global([data-theme="dark"]) .tag.db {
    background: rgba(30, 130, 60, 0.2);
    border-color: rgba(30, 130, 60, 0.5);
    color: #7adc9e;
  }
  :global([data-theme="dark"]) .tag.origin {
    background: rgba(120, 40, 180, 0.2);
    border-color: rgba(120, 40, 180, 0.5);
    color: #c09cff;
  }
  :global([data-theme="dark"]) .tag.header {
    background: rgba(160, 120, 0, 0.2);
    border-color: rgba(160, 120, 0, 0.5);
    color: #e0c050;
  }
  :global([data-theme="dark"]) .tag.session {
    background: rgba(180, 30, 60, 0.2);
    border-color: rgba(180, 30, 60, 0.5);
    color: #f08090;
  }
</style>
