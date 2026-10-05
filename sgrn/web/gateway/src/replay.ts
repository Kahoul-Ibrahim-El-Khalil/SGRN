import ReplayApp from "./ReplayApp.svelte";
import "./styles/shared.css";
import "./styles/global.css";

const app = new ReplayApp({
  target: document.getElementById("app")!,
});

export default app;
