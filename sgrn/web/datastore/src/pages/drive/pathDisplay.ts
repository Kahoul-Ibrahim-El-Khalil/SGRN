// @/pages/drive/pathDisplay.ts
//
// Presentation transform for domain namespaces. Storage paths are always
// canonical ("/domains/<name>/..."), only UI rendering maps them to "@<name>/..."
// so a domain reads like a real handle ("@acme") instead of "domains/".

export const DOMAIN_PATH_PREFIX = "/domains/";

export function toDisplayDrivePath(p: string | null | undefined): string {
    if (!p) return "";
    return p.startsWith(DOMAIN_PATH_PREFIX) ? "@" + p.slice(DOMAIN_PATH_PREFIX.length) : p;
}

/** Extracts the domain segment of a canonical path, "" if not domain-scoped. */
export function domainOfPath(p: string | null | undefined): string {
    if (!p || !p.startsWith(DOMAIN_PATH_PREFIX)) return "";
    const rest = p.slice(DOMAIN_PATH_PREFIX.length);
    return rest.slice(0, Math.max(0, rest.indexOf("/")));
}

/** "@acme" when the path lives inside the /domains/<name> namespace, else null. */
export function domainDisplaySegment(p: string | null | undefined): { name: string; path: string } | null {
    const domain = domainOfPath(p);
    return domain ? { name: "@" + domain, path: DOMAIN_PATH_PREFIX + domain } : null;
}
