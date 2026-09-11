<script lang="ts">
  import Sidebar from "../components/Sidebar.svelte";
  import DocsContent from "../components/DocsContent.svelte";
  
  export let params: { doc?: string } = {};

  $: docName = params.doc || "index";
</script>

<div class="docs-layout">
  <Sidebar />
  <main class="docs-content">
    <div class="docs-page">
      <DocsContent {docName} />
    </div>
  </main>
</div>

<style>
  .docs-layout {
    display: flex;
    flex: 1;
    overflow: hidden;
    height: 100%;
  }

  .docs-content {
    flex: 1;
    overflow-y: auto;
    background: var(--surface-bright);
    padding: 24px 48px 48px;
}

.docs-page {
    width: min(1400px, 100%);
    margin: 0 auto;
    line-height: 1.65;
    font-size: 14px;
    color: var(--text);
}
  .docs-page :global(h2) {
    font-size: 18px;
    font-weight: 700;
    margin: 24px 0 12px;
    border-bottom: 1px solid var(--surface-border);
    padding-bottom: 6px;
  }

  .docs-page :global(h3) {
    font-size: 14px;
    font-weight: 700;
    margin: 20px 0 10px;
  }

  .docs-page :global(h4) {
    font-size: 12px;
    font-weight: 700;
    margin: 16px 0 8px;
  }

  .docs-page :global(p) {
    margin: 0 0 12px;
  }

  .docs-page :global(code) {
    font-family: var(--mono);
    font-size: 11px;
    background: var(--surface);
    padding: 2px 4px;
    border-radius: 3px;
    border: 1px solid var(--surface-border);
  }

  .docs-page :global(pre) {
    background: var(--surface);
    border: 1px solid var(--surface-border);
    padding: 12px;
    border-radius: 4px;
    overflow-x: auto;
    margin: 12px 0;
  }

  .docs-page :global(pre code) {
    background: none;
    border: none;
    padding: 0;
    font-size: 11px;
  }

  .docs-page :global(ul), .docs-page :global(ol) {
    margin: 0 0 12px;
    padding-left: 20px;
  }

  .docs-page :global(li) {
    margin-bottom: 4px;
  }

  .docs-page :global(.callout) {
    padding: 12px 16px;
    border-left: 4px solid var(--accent);
    background: var(--accent-light);
    margin: 16px 0;
    border-radius: 0 4px 4px 0;
  }

  .docs-page :global(.callout-warn) {
    border-left-color: var(--warning);
    background: rgba(160, 80, 0, 0.08);
  }
</style>
