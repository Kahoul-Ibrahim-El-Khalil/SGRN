// Row types for the generated CRUD views, derived from the generated wire
// types (@sgrn/datastore/crud-responses) so a schema change that regenerates
// those files breaks compilation here instead of silently at runtime.
//
// The generated rows mark every column optional because SQL NULL and "column
// not selected" look the same on the wire. On the wire a NULL arrives as
// `null` (MessagePack nil), which the generated `?: T` does not admit, so the
// derived types add `| null` and re-require the columns every row always has.
import type { DomainsRow, UsersRow } from "@sgrn/datastore/crud-responses";

type Wire<T> = { [K in keyof T]?: T[K] | null };

export const ENCRYPTION_POLICIES = ["none", "optional", "required"] as const;
export type EncryptionPolicy = (typeof ENCRYPTION_POLICIES)[number];

/** Row of GET /api/v1/domains. */
export type DomainRecord = Wire<Omit<DomainsRow, "id" | "name">> & {
    id: number;
    name: string;
};

/** Row of GET /api/v1/users. */
export type UserRecord = Wire<Omit<UsersRow, "id" | "first_name" | "family_name" | "email">> & {
    id: number;
    first_name: string;
    family_name: string;
    email: string;
};

/** The policy a domain actually enforces (the column is nullable; "optional" is the default). */
export function policyOf(t_domain: Pick<DomainRecord, "encryption_policy">): EncryptionPolicy {
    const policy = t_domain.encryption_policy;
    return policy === "none" || policy === "required" ? policy : "optional";
}
