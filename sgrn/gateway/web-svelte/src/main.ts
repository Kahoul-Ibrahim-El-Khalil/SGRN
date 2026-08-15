import App from "./App.svelte";
import "./styles/shared.css";
import "./styles/global.css";

const app = new App({
  target: document.getElementById("app")!,
});

export default app;
