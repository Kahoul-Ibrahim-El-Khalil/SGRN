// config.ts

export const BASE_PATH =
  (window as any).__SGRN_BASE__ ??
  document.querySelector("base")?.getAttribute("href") ??
  "";

export function api(path: string): string {
  return `${BASE_PATH}${path}`;
}

export function ws(path: string): string {
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  return `${scheme}://${location.host}${BASE_PATH}${path}`;
}
