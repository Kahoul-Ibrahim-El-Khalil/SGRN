<script lang="ts">
  import DOC_INDEX_MARKDOWN from "@docs/index.md";
  import DOC_ARCH_MARKDOWN from "@docs/arch.md";
  import DOC_CONDIF_MARKDOWN from "@docs/config.md";
  import DOC_EIP_MARKDOWN from "@docs/eip.md";
  import DOC_MODBUS_MARKDOWN from "@docs/modbus.md";
  import DOC_OPCUA_MARKDOWN from "@docs/opcua.md";
  import DOC_PERSITENCE_MARKDOWN from "@docs/persistence.md";
  import DOC_REST_MARKDOWN from "@docs/rest.md";
  import DOC_S7_MARKDOWN from "@docs/s7.md";
  import DOC_SECURITY_MARKDOWN from "@docs/security.md";
  import DOC_WEBSOCKET_MAKRDOWN from "@docs/websocket.md";

  export let docName: string = "index";

  const docs: Record<string, any> = {
    index: DOC_INDEX_MARKDOWN,
    arch: DOC_ARCH_MARKDOWN,
    config: DOC_CONDIF_MARKDOWN,
    eip: DOC_EIP_MARKDOWN,
    modbus: DOC_MODBUS_MARKDOWN,
    opcua: DOC_OPCUA_MARKDOWN,
    persistence: DOC_PERSITENCE_MARKDOWN,
    rest: DOC_REST_MARKDOWN,
    s7: DOC_S7_MARKDOWN,
    security: DOC_SECURITY_MARKDOWN,
    websocket: DOC_WEBSOCKET_MAKRDOWN,
  };

  $: component = docs[docName] || null;
</script>

{#if component}
  <svelte:component this={component} />
{/if}
