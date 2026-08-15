import { writable } from "svelte/store";

const initialTheme =
  typeof localStorage !== "undefined"
    ? localStorage.getItem("sgrn-theme") || "light"
    : "light";
export const theme = writable(initialTheme);

theme.subscribe((value) => {
  if (typeof localStorage !== "undefined") {
    localStorage.setItem("sgrn-theme", value);
  }
  if (typeof document !== "undefined") {
    document.documentElement.setAttribute("data-theme", value);
  }
});
