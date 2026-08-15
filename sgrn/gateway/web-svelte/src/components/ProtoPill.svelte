<script lang="ts">
  export let proto: string;
</script>

<span class="proto-pill {proto}">
  {proto}
</span>

<style>
  .proto-pill {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 2px 8px;
    border-radius: 10px;
    font-size: 10px;
    font-weight: 700;
    letter-spacing: 0.3px;
    white-space: nowrap;
  }
  .proto-pill.S7 {
    background: #dbe9ff;
    color: #1a3a6e;
  }
  .proto-pill.HTTP {
    background: #fdf3e0;
    color: #7a4200;
  }
  .proto-pill.WebSocket {
    background: #eadeff;
    color: #4a1ea0;
  }
  .proto-pill.Modbus {
    background: #dffaed;
    color: #155b35;
  }
  .proto-pill.OPC-UA {
    background: #fde8e8;
    color: #8c1515;
  }
  .proto-pill.EtherNet\/IP {
    background: #e8f4f8;
    color: #0d4a62;
  }

  :global([data-theme="dark"]) .proto-pill.S7 {
    background: rgba(30, 80, 180, 0.25);
    color: #7ab0ff;
  }
  :global([data-theme="dark"]) .proto-pill.HTTP {
    background: rgba(180, 100, 0, 0.2);
    color: #ffb86c;
  }
  :global([data-theme="dark"]) .proto-pill.WebSocket {
    background: rgba(120, 60, 220, 0.25);
    color: #c09cff;
  }
  :global([data-theme="dark"]) .proto-pill.Modbus {
    background: rgba(30, 140, 70, 0.2);
    color: #6fda9e;
  }
  :global([data-theme="dark"]) .proto-pill.OPC-UA {
    background: rgba(200, 30, 30, 0.2);
    color: #ff8080;
  }
  :global([data-theme="dark"]) .proto-pill.EtherNet\/IP {
    background: rgba(0, 100, 150, 0.2);
    color: #7dd3ea;
  }
</style>
