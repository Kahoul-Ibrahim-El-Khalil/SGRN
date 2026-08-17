function normalizeBasePath(t_base_path: string): string {
  if (!t_base_path || t_base_path === "/") {
    return "";
  }
  return t_base_path.endsWith("/") ? t_base_path.slice(0, -1) : t_base_path;
}

let basePathOverride: string | null = null;

export function setBasePath(t_base_path: string | null): void {
  basePathOverride = t_base_path === null ? null : normalizeBasePath(t_base_path);
}

export function getBasePath(): string {
  if (basePathOverride !== null) {
    return basePathOverride;
  }

  if (typeof window !== "undefined") {
    const win = window as Window & { __SGRN_BASE__?: string };
    if (typeof win.__SGRN_BASE__ === "string") {
      return normalizeBasePath(win.__SGRN_BASE__);
    }

    const baseHref = document.querySelector("base")?.getAttribute("href");
    if (baseHref) {
      return normalizeBasePath(baseHref);
    }
  }

  return "";
}

export function api(path: string): string {
  return `${getBasePath()}${path}`;
}

export function ws(path: string): string {
  const locationLike =
    typeof window !== "undefined" ? window.location : globalThis.location;
  const scheme = locationLike?.protocol === "https:" ? "wss" : "ws";
  const host = locationLike?.host ?? "localhost";
  return `${scheme}://${host}${getBasePath()}${path}`;
}
