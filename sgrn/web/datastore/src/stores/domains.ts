// Shared domain registry. Four panels read the domain list (user editor,
// domain registry, encryption policies, registration); they now share this one
// cached copy, and a mutation anywhere refreshes it everywhere.
import { listDomains } from "@/backend/api/domains";
import { createRemoteStore } from "@/stores/createRemoteStore";
import type { DomainRecord } from "@/types/records";

export const domainsStore = createRemoteStore<DomainRecord[]>([], () => listDomains());
