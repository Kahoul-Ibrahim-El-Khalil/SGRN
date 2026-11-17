import { domainsStore } from "@/stores/domains";

export { domainsStore };

/**
 * Drop every cached server value. Called on sign-in and sign-out so one
 * account's data (e.g. its domain list) can never show up for the next.
 */
export function resetSharedStores(): void {
    domainsStore.reset();
}
