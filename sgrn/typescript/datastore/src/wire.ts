export const JSON_CONTENT_TYPE = "application/json";

/**
 * A JSON document carried as a *string* inside a JSON body.
 *
 * The C++ side models these fields as `sgrn::json::RawJson` (permissions,
 * metadata, system-config values). Those fields are JSON *text*, not a nested
 * structure, so they must be stringified on the client. Build them with
 * `rawJson()`.
 *
 * The brand is a plain string-keyed property (not a `unique symbol`) on
 * purpose: the same file can be loaded through two paths (workspace source
 * and node_modules), and a unique symbol would make the two copies
 * incompatible with each other.
 */
export type RawJsonString<T = unknown> = string & {
  readonly __sgrnRawJson: T;
};

export function rawJson<T>(t_value: T): RawJsonString<T> {
  return JSON.stringify(t_value) as RawJsonString<T>;
}

/**
 * Spread into a `fetch` / `authenticatedFetch` init to send a typed DTO as
 * JSON:
 *
 *   authenticatedFetch(url, { method: "POST", ...jsonBody<SetStatusRequest>({ status }) })
 *
 * `JSON.stringify` omits `undefined` fields (so the C++ default / empty
 * optional applies); an explicit `null` is still sent as null.
 */
export function jsonBody<T extends object>(
  t_body: T,
): { headers: Record<string, string>; body: BodyInit } {
  return {
    // Accept is explicit: the server otherwise negotiates its binary default
    // (MessagePack) for callers that send no preference, which a browser
    // cannot decode. Every JSON request therefore asks for JSON.
    headers: { "Content-Type": JSON_CONTENT_TYPE, Accept: JSON_CONTENT_TYPE },
    body: JSON.stringify(t_body) as unknown as BodyInit,
  };
}
