# SGRN Datastore: Admin Route Authorization Audit

This document audits all 38 routes under `/api/v1/admin/*` across `AdminApiHandler` and `StorageAdminHandler` to classify their tenant boundary, identify cross-organisation leakage vectors, and specify the concrete authorization enforcement applied.

## Audit Matrix

| # | Route | Method | Handler Class | Caller (Pre-Hardening) | Target Organisation (Pre-Hardening) | Org Source (Pre-Hardening) | Security Assessment | Hardened Scoping & Authorization Rule |
|---|---|---|---|---|---|---|---|---|
| 1 | `/api/v1/admin/status` | GET | `AdminApiHandler` | Any Admin | N/A (Server health) | None | System status info only (no tenant data) | Allowed for all admins. No tenant data leaked. |
| 2 | `/api/v1/admin/users` | GET | `AdminApiHandler` | Any Admin | Fleet-wide (`mapper.findAll()`) | None (Unscoped) | **CRITICAL LEAK**: Tenant admin sees all users across all organisations | Enforce `WHERE organisation = session_org`. Platform admins may view all if no org query parameter is set. |
| 3 | `/api/v1/admin/users/register` | POST | `AdminApiHandler` | Any Admin | From payload `body.organisation` | Request payload | **CRITICAL LEAK**: Tenant admin can register users into foreign organisations | Force `payload.organisation = session_org`. Platform admins can explicitly target an organisation. |
| 4 | `/api/v1/admin/automated-services/register` | POST | `AdminApiHandler` | Any Admin | Creator organisation | Session | Properly scoped | Enforce `organisation = session_org` (platform admin may override). |
| 5 | `/api/v1/admin/metaprobe/sessions` | GET | `AdminApiHandler` | Any Admin | Global active sessions | None (Unscoped) | **LEAK**: Active sessions of all users in memory leaked | Filter returned sessions by caller `session_org`. Platform admins see all. |
| 6 | `/api/v1/admin/automated-services/rotate-token` | POST | `AdminApiHandler` | Any Admin | Service organisation verified | Session | Properly scoped (`owner_org == org`) | Preserved. Re-verified against session organisation. |
| 7 | `/api/v1/admin/permissions` | GET | `AdminApiHandler` | Any Admin | All orgs if omitted or any `?organisation=` | Request query param | **CRITICAL LEAK**: Tenant admin can query permissions of any other tenant | For non-platform admins, force `organisation = session_org`. Reject `?organisation=` if it does not equal session org. |
| 8 | `/api/v1/admin/permissions` | POST | `AdminApiHandler` | Any Admin | Target user's org | Request target user | **HIGH RISK**: Org admin can grant permissions to users in another tenant | Verify target user belongs to `session_org`. Set `organisation = session_org`. |
| 9 | `/api/v1/admin/permissions` | DELETE | `AdminApiHandler` | Any Admin | Unchecked if `?id=` or from query param | Request query param | **HIGH RISK**: Org admin can delete permission grants of other tenants | Verify permission row belongs to `session_org`. For `?id=`, check `p.organisation = session_org`. |
| 10 | `/api/v1/admin/roles` | GET | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WHERE organisation = $1`) | Preserved. |
| 11 | `/api/v1/admin/roles` | POST | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`INSERT ... organisation = session_org`) | Preserved. |
| 12 | `/api/v1/admin/roles/assign` | POST | `AdminApiHandler` | Any Admin | Target role / user / service | Request payload | **HIGH RISK**: No verification that role_id or user/service belongs to tenant | Verify `role_id` and target `user_id`/`automated_service_id` belong to `session_org`. |
| 13 | `/api/v1/admin/roles/revoke` | POST | `AdminApiHandler` | Any Admin | Target role / user / service | Request payload | **HIGH RISK**: No verification that role_id or user/service belongs to tenant | Verify `role_id` and target `user_id`/`automated_service_id` belong to `session_org`. |
| 14 | `/api/v1/admin/audit-logs` | GET | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WHERE organisation = $1`) | Preserved. Platform admin can query across orgs if specified. |
| 15 | `/api/v1/admin/audit-logs` | DELETE | `AdminApiHandler` | Any Admin | Session organisation | Session | **SECURITY CONTROL VIOLATION**: Tenant admin can purge audit logs | **403 Forbidden** for tenant admins. Platform admin only (`security.system_admin_emails`), enforces retention floor (>= 90 days), writes `audit.purged` record. |
| 16 | `/api/v1/admin/quotas` | GET | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WHERE organisation = $1`) | Preserved. |
| 17 | `/api/v1/admin/quotas/organisation` | PUT | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WHERE name = session_org`) | Preserved. |
| 18 | `/api/v1/admin/webhooks` | GET | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WebhookService::getWebhooks(org)`) | Preserved. |
| 19 | `/api/v1/admin/webhooks` | POST | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WebhookService::createWebhook(org, ...)`) | Preserved. |
| 20 | `/api/v1/admin/automated-services/{id}/metadata` | PATCH | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WHERE id = $2 AND organisation = $3`) | Preserved. |
| 21 | `/api/v1/admin/roles/{id}` | PUT | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WHERE organisation = $1 AND id = $2`) | Preserved. |
| 22 | `/api/v1/admin/roles/{id}` | DELETE | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WHERE organisation = $1 AND id = $2`) | Preserved. |
| 23 | `/api/v1/admin/quotas/user/{id}` | PUT | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WHERE organisation = $1 AND id = $2`) | Preserved. |
| 24 | `/api/v1/admin/quotas/service/{id}` | PUT | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WHERE organisation = $1 AND id = $2`) | Preserved. |
| 25 | `/api/v1/admin/webhooks/{id}` | DELETE | `AdminApiHandler` | Any Admin | Session organisation | Session | Properly scoped (`WebhookService::deleteWebhook(org, id)`) | Preserved. |
| 26 | `/api/v1/admin/storage/overview` | GET | `StorageAdminHandler` | Any Admin | Fleet-wide | None (Unscoped) | **LEAK**: DB objects and files census includes all organisations | Org admin scopes DB file/object census to `session_org`. Platform admin views fleet-wide. |
| 27 | `/api/v1/admin/storage/orphans` | GET | `StorageAdminHandler` | Any Admin | S3 bucket & DB objects | None (System infrastructure) | Infrastructure diagnostics | Platform admin or tenant admin (DB orphan cross-check). |
| 28 | `/api/v1/admin/storage/orphans/purge` | POST | `StorageAdminHandler` | Any Admin | S3 bucket | Request payload | **HIGH DESTRUCTION RISK**: Org admin can delete storage objects | **403 Forbidden** for tenant admins. Gated strictly to platform admins (`security.system_admin_emails`). |
| 29 | `/api/v1/admin/storage/search` | GET | `StorageAdminHandler` | Any Admin | DB object + all referencing files | None (Unscoped) | **LEAK**: Referencing files from other tenants are listed in response | Tenant admin only sees referencing files belonging to their `session_org`. Platform admin sees all. |
| 30 | `/api/v1/admin/system/config` | GET | `StorageAdminHandler` | Any Admin | Global daemon config | None (System-wide) | **INFO DISCLOSURE**: Tenant admin reads server configuration | **403 Forbidden** for tenant admins. Gated strictly to platform admins (`security.system_admin_emails`). |
| 31 | `/api/v1/admin/system/config` | PUT | `StorageAdminHandler` | Any Admin | Global daemon config | None (System-wide) | **HIGH DESTRUCTION RISK**: Tenant admin can rewrite host configuration | **403 Forbidden** for tenant admins. Gated strictly to platform admins (`security.system_admin_emails`). |
| 32 | `/api/v1/admin/storage/formats` | GET | `StorageAdminHandler` | Any Admin | Global format registry | None (System-wide) | Global file extensions registry | Read-only format registry allowed. |
| 33 | `/api/v1/admin/storage/formats` | POST | `StorageAdminHandler` | Any Admin | Global format registry | None (System-wide) | Upsert file format extension | Allowed for admins (format curation). |
| 34 | `/api/v1/admin/storage/formats` | DELETE | `StorageAdminHandler` | Any Admin | Global format registry | None (System-wide) | Delete file format extension | Allowed for admins (refused if referenced). |
| 35 | `/api/v1/admin/quotas` | GET | `StorageAdminHandler` | Any Admin | All users, services, or organisations | Query param (Unscoped) | **CRITICAL LEAK**: Tenant admin lists all users/services/orgs across platform | For `user` and `service`, bind `WHERE organisation = session_org`. For `organisation`, return only `session_org`. Platform admins see all. |
| 36 | `/api/v1/admin/quotas` | PUT | `StorageAdminHandler` | Any Admin | Any target user, service, or organisation | Request payload | **CRITICAL TAMPERING**: Tenant admin can alter quotas of other tenants | Require target user/service/org to belong to `session_org`. Platform admins can tune any. |
| 37 | `/api/v1/admin/analytics/overview` | GET | `StorageAdminHandler` | Any Admin | Fleet-wide counts, uploaders, recent files | None (Unscoped) | **CRITICAL LEAK**: Shows all files, uploaders, and stats across all tenants | For tenant admins, filter counts, bytes, 30d series, top uploaders, and recent files strictly by `session_org`. Platform admins see fleet-wide. |
| 38 | `/api/v1/admin/analytics/breakdown` | GET | `StorageAdminHandler` | Any Admin | Fleet-wide breakdown slices | Query param (Unscoped) | **CRITICAL LEAK**: Tenant admin views other tenants' activity and storage | Filter all breakdowns (domain, organisation, user, service) strictly by `session_org`. Platform admins see all. |

## Platform Administrator Identification (Pre-Prompt 4)

Until the platform administrator role is added to the database schema in Task 4, platform administrator privileges are determined by verifying whether the authenticated caller's email exists in:
1. The configuration path `custom_config.security.system_admin_emails` in `sgrn.json`, or
2. The environment variable `SGRN_SYSTEM_ADMIN_EMAILS` (comma-separated list).

If the caller's email is present in this list, they are recognized as a Platform Administrator. Otherwise, they are treated as a tenant Organisation Administrator and restricted to their `session_org`.
