# SGRN Datastore: Authorization Matrix

This document defines the route authorization classification across all `/api/v1/admin/*` endpoints in `AdminApiHandler` and `StorageAdminHandler`. Each route is protected by a matching filter (`PlatformAdminFilter`, `OrgAdminFilter`, or `DomainAdminFilter`) wrapping `ScopeFilter` and backed by `AdminLevelCache` / `core.admin_level()`.

## Admin Levels Summary

| Level        | Identifier | Numeric | Description                                                                                             |
| ------------ | ---------- | ------- | ------------------------------------------------------------------------------------------------------- |
| **NONE**     | `NONE`     | 0       | Regular authenticated user; no admin capabilities.                                                      |
| **DOMAIN**   | `DOMAIN`   | 1       | Scoped administrator over a specific domain subtree.                                                    |
| **ORG**      | `ORG`      | 2       | Organisation administrator; full tenant control within their organisation.                              |
| **PLATFORM** | `PLATFORM` | 3       | Fleet-wide platform superadministrator (`is_platform_admin = true`). Cross-tenant operations permitted. |

---

## Route Classification Matrix

### AdminApiHandler (`sgrn/lib/datastore/handlers/admin.hpp`)

| Route                                            | Method | Handler Method                         | Registered Filter                       | Required Level | Resolver            | Scoping & Security Notes                                                                       |
| ------------------------------------------------ | ------ | -------------------------------------- | --------------------------------------- | -------------- | ------------------- | ---------------------------------------------------------------------------------------------- |
| `/api/v1/admin/status`                           | GET    | `handleGetStatus`                      | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Server status and health metrics within caller's tenant context.                               |
| `/api/v1/admin/users`                            | GET    | `handleGetUsers`                       | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Tenant users list; scoped strictly to caller's `organisation`. Platform admins can view all.   |
| `/api/v1/admin/users/register`                   | POST   | `handleRegisterUser`                   | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Registers user. Non-platform admin forced to caller `session_org`.                             |
| `/api/v1/admin/automated-services/register`      | POST   | `handleRegisterAutomatedService`       | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Service created in caller's organisation.                                                      |
| `/api/v1/endpoints`                              | GET    | `handleGetEndpoints`                   | `UserAuthFilter`                        | NONE           | None                | Public discovery of available endpoints for current user session.                              |
| `/api/v1/admin/metaprobe/sessions`               | GET    | `handleGetMetaProbeSessions`           | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Active sessions filtered to caller's `session_org`.                                            |
| `/api/v1/admin/automated-services/rotate-token`  | POST   | `handleRotateAutomatedServiceToken`    | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Rotates token; target service validated to belong to caller's organisation.                    |
| `/api/v1/admin/permissions`                      | GET    | `handleListPermissions`                | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Lists permissions; query parameter restricted to `session_org` for non-platform admins.        |
| `/api/v1/admin/permissions`                      | POST   | `handleGrantPermission`                | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Grants permission; target user/role must belong to `session_org`. Escalation guards apply.     |
| `/api/v1/admin/permissions`                      | DELETE | `handleRevokePermission`               | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Revokes permission; target row must belong to `session_org`.                                   |
| `/api/v1/admin/roles`                            | GET    | `handleGetRoles`                       | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Lists tenant roles.                                                                            |
| `/api/v1/admin/roles`                            | POST   | `handleCreateRole`                     | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Creates tenant role.                                                                           |
| `/api/v1/admin/roles/assign`                     | POST   | `handleAssignRole`                     | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Assigns role to user/service; both target and role must belong to caller `session_org`.        |
| `/api/v1/admin/roles/revoke`                     | POST   | `handleRevokeRole`                     | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Revokes role from user/service; target must belong to caller `session_org`.                    |
| `/api/v1/admin/audit-logs`                       | GET    | `handleGetAuditLogs`                   | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Reads audit logs scoped to caller `session_org`.                                               |
| `/api/v1/admin/audit-logs`                       | DELETE | `handlePurgeAuditLogs`                 | `UserAuthFilter`, `PlatformAdminFilter` | **PLATFORM**   | None                | **Destructive operation**: fleet-wide audit log purge. Restricted strictly to platform admins. |
| `/api/v1/admin/quotas`                           | GET    | `handleGetQuotas`                      | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Retrieves quotas for caller `session_org`.                                                     |
| `/api/v1/admin/quotas/organisation`              | PUT    | `handleUpdateOrgQuota`                 | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Modifies quota of caller's own organisation.                                                   |
| `/api/v1/admin/webhooks`                         | GET    | `handleListWebhooks`                   | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Lists webhooks for caller `session_org`.                                                       |
| `/api/v1/admin/webhooks`                         | POST   | `handleRegisterWebhook`                | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Registers webhook in caller `session_org`.                                                     |
| `/api/v1/admin/automated-services/{id}/metadata` | PATCH  | `handleUpdateAutomatedServiceMetadata` | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Updates automated service metadata; checked for `organisation = session_org`.                  |
| `/api/v1/admin/roles/{id}`                       | PUT    | `handleUpdateRole`                     | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Modifies tenant role; checked for `organisation = session_org`.                                |
| `/api/v1/admin/roles/{id}`                       | DELETE | `handleDeleteRole`                     | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Deletes tenant role; checked for `organisation = session_org`.                                 |
| `/api/v1/admin/quotas/user/{id}`                 | PUT    | `handleUpdateUserQuota`                | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Updates quota for user in `session_org`.                                                       |
| `/api/v1/admin/quotas/service/{id}`              | PUT    | `handleUpdateServiceQuota`             | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Updates quota for service in `session_org`.                                                    |
| `/api/v1/admin/webhooks/{id}`                    | DELETE | `handleDeleteWebhook`                  | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Deletes webhook belonging to `session_org`.                                                    |

---

### StorageAdminHandler (`sgrn/lib/datastore/handlers/storage-admin.hpp`)

| Route                                 | Method | Handler Method             | Registered Filter                       | Required Level | Resolver            | Scoping & Security Notes                                                                       |
| ------------------------------------- | ------ | -------------------------- | --------------------------------------- | -------------- | ------------------- | ---------------------------------------------------------------------------------------------- |
| `/api/v1/admin/storage/overview`      | GET    | `handleOverview`           | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Overview metrics scoped to `session_org` (fleet-wide for platform admin).                      |
| `/api/v1/admin/storage/orphans`       | GET    | `handleOrphans`            | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Read-only orphan inspection.                                                                   |
| `/api/v1/admin/storage/orphans/purge` | POST   | `handlePurgeOrphans`       | `UserAuthFilter`, `PlatformAdminFilter` | **PLATFORM**   | None                | **Destructive operation**: physical object removal in S3/storage backend. Platform admin only. |
| `/api/v1/admin/storage/search`        | GET    | `handleSearch`             | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Storage object search; referencing files scoped to caller `session_org`.                       |
| `/api/v1/admin/system/config`         | GET    | `handleGetSystemConfig`    | `UserAuthFilter`, `PlatformAdminFilter` | **PLATFORM**   | None                | Reads global host daemon configuration. Platform admin only.                                   |
| `/api/v1/admin/system/config`         | PUT    | `handleUpdateSystemConfig` | `UserAuthFilter`, `PlatformAdminFilter` | **PLATFORM**   | None                | Modifies global host daemon configuration. Platform admin only.                                |
| `/api/v1/admin/storage/formats`       | GET    | `handleListFormats`        | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Global format extension registry list.                                                         |
| `/api/v1/admin/storage/formats`       | POST   | `handleUpsertFormat`       | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Format extension upsert.                                                                       |
| `/api/v1/admin/storage/formats`       | DELETE | `handleDeleteFormat`       | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Format extension deletion.                                                                     |
| `/api/v1/admin/quotas`                | GET    | `handleListQuotas`         | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Storage quotas list scoped to `session_org`.                                                   |
| `/api/v1/admin/quotas`                | PUT    | `handleUpdateQuota`        | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Updates storage quota for user/service/org within `session_org`.                               |
| `/api/v1/admin/analytics/overview`    | GET    | `handleAnalyticsOverview`  | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Analytics overview scoped to `session_org`.                                                    |
| `/api/v1/admin/analytics/breakdown`   | GET    | `handleAnalyticsBreakdown` | `UserAuthFilter`, `OrgAdminFilter`      | ORG            | `resolveSessionOrg` | Analytics breakdown slices scoped to `session_org`.                                            |

---

## Privilege Escalation Defense Rules

1. **Self-Grant Prevention**: An admin cannot grant permissions or roles to themselves.
2. **Above-Level Grant Prevention**: An admin cannot grant a scope or role level higher than their own (e.g. Org admin cannot grant Platform admin or assign cross-tenant scopes).
3. **Subtree Boundary**: Domain admins cannot assign roles or permissions outside their domain hierarchy subtree.
4. **Last Platform Admin Protection**: An admin cannot revoke the last active platform administrator or delete their assignment.
5. **Auditing**: All grant, revoke, and elevation attempts (both succeeded and rejected) are recorded via `AuditLogger`.
