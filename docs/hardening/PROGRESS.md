# SGRN Datastore: Hardening Progress

## Task Status Table

| Task | Name | Status | Branch | Commit Range | Acceptance Evidence | Open Risks |
|---|---|---|---|---|---|---|
| 1 | Lock down Postgres, nginx, systemd | ready for verification | datastore/hardening | working tree | `verify_test` unit tests added; configs & service files hardened | Needs live systemd restart drill |
| 2 | Build hardening and sanitizer CI | ready for verification | datastore/hardening | working tree | `build_hardening.cmake`, CI matrix (Release, ASan+UBSan, TSan), `asan-scratch-test.cpp` | Needs user build & sanitizer run |
| 3 | Fix cross-organisation leaks and audit purge | done | datastore/hardening | working tree | `docs/admin-route-audit.md` (38 routes, 0 unscoped), `cross_org_auth_test` (7/7 passed) | None |
| 12 | Fuzzing and regression suite (start early) | todo | datastore/hardening | - | - | - |
| 4 | Schema for platform/org/domain admins | done | datastore/hardening | working tree | `migrations/003_scoped_admins.sql`, `scoped_admins_test` (ALL CHECKS PASSED) | None |
| 5 | ScopeFilter and route classification | in progress | datastore/hardening | - | - | - |
| 6 | Tamper-evident audit log | todo | datastore/hardening | - | - | - |
| 7 | Crypto core library | todo | datastore/hardening | - | - | - |
| 8 | Encrypted storage & SQLite key store | todo | datastore/hardening | - | - | - |
| 9 | Key lifecycle, crypto-shred, backups | todo | datastore/hardening | - | - | - |
| 10 | Authentication and sessions | todo | datastore/hardening | - | - | - |
| 11 | Editing, versions and text deltas | todo | datastore/hardening | - | - | - |
| 13 | Threat model and operations evidence | todo | datastore/hardening | - | - | - |

---

## Task 1: Current Behavior Summary (Pre-edit)

- **configs/postgres/pg_hba.conf**: Permits local Unix domain socket and IPv4 loopback (127.0.0.1/32) authentication using insecure `trust`, alongside a wide-open `host all all 0.0.0.0/0` rule using SCRAM-SHA-256 for all databases and users.
- **configs/postgres/postgresql.conf**: Binds PostgreSQL to all network interfaces (`listen_addresses = '*'`), leaves SSL disabled (`#ssl = off`), TLS min protocol commented out at TLSv1.2, and connection/disconnection logging disabled.
- **configs/nginx/sites-enabled/sgrn.conf & nginx.conf**: TLS configuration allows TLSv1.2 with default broad ciphers (`HIGH:!aNULL:!MD5`), lacks HSTS, X-Content-Type-Options, Referrer-Policy, and strict CSP headers, and sets `client_max_body_size 0` in `/minio/`.
- **configs/systemd/*.service**: Service units (`SGRN-datastore`, `SGRN-garage`, `SGRN-postgres`, `SGRN-nginx`, `sgrn`) run without seccomp system call filtering (`SystemCallFilter`), without dropped capabilities (`CapabilityBoundingSet`), and with relaxed filesystem isolation (`ProtectSystem=full` or unset, `ProtectHome=false`).
- **include/sgrn/datastore/bootstrap/config-gen.hpp & verify.hpp**: Bootstrap generation and verification logic check for basic file existence and unit template drift, but do not fail on `trust` or `0.0.0.0/0` in `pg_hba.conf`, do not check for `.env` mode 0600 permissions, and do not fail on default self-signed TLS certificates unless `--allow-insecure` is supplied.

---

## Task 2: Current Behavior Summary (Pre-edit)

- **CMakeLists.txt**: Left `CMAKE_BUILD_TYPE` unset by default without a fallback to `Release`, omitting project-wide compiler security flags.
- **cmake/debug_flags.cmake**: Handled only diagnostic boolean flags, disabling them under `RELEASE`, but lacking integration with modern hardening flags or sanitizers.
- **cmake/sgrn_datastore_core.cmake**: Contained standard compiler flags (`-Wall -Wextra`) and a legacy `SANITIZE` boolean for address/undefined without stack canary, fortify source, stack clash protection, or PIE/Full RELRO options.
- **.github/workflows/ci.yml**: Implemented a single monolithic release build on Linux without a multi-target sanitizer matrix (ASan, UBSan, TSan), static analysis (clang-tidy, cppcheck), or SBOM generation.
- **tests/**: Lacked a dedicated verification scratch test demonstrating that AddressSanitizer correctly traps out-of-bounds heap reads.

---

## Task 2: Completed Changes

1. **`cmake/build_hardening.cmake` (New)**:
   - Configured `CMAKE_BUILD_TYPE=Release` default when unset.
   - Enforced compiler flags: `-fstack-protector-strong`, `-Wformat -Wformat-security`, `-fstack-clash-protection`, `-fcf-protection` (on x86_64).
   - Enforced macro `-D_FORTIFY_SOURCE=3` in `Release` configurations.
   - Enforced `-fPIE` and `-pie -Wl,-z,relro,-z,now` (Full RELRO and Bind Now) on executables and shared libraries.
   - Added `SGRN_SANITIZE=asan|ubsan|tsan|asan,ubsan` option, enforcing mutual exclusivity (e.g. rejecting TSan combined with ASan).

2. **Integration into Build System**:
   - `CMakeLists.txt`: included `cmake/build_hardening.cmake`.
   - `cmake/targets.cmake`: wired `sgrn_apply_target_hardening` into all component libraries and executables.
   - `sgrn/lib/datastore/cmake/sgrn_datastore_core.cmake`: applied hardening flags to `sgrn_datastore_core` interface and wired `SGRN_SANITIZE`.
   - `sgrn/lib/datastore/cmake/debug_flags.cmake`: synchronized with `Release` mode checks.

3. **Dependency Pinning & SBOM**:
   - `cmake/deps/FetchDeps.cmake`: pinned OpENer to upstream release `2.3.1` (replacing mutable `master`); added `URL_HASH` support to `sgrn_fetch_source`.
   - `.github/workflows/ci.yml`: added `cyclonedx-sbom` job generating CycloneDX SBOM JSON artifact.

4. **Static Analysis & CI**:
   - `.clang-tidy`: enabled `bugprone-*`, `cert-*`, and `clang-analyzer-*` checks.
   - `.github/workflows/ci.yml`: configured test matrix for `Release`, `ASan + UBSan`, and `TSan`; added `static-analysis` job running `clang-tidy` and `cppcheck`.

5. **ASan Scratch Test**:
   - Added `sgrn/lib/datastore/tests/asan-scratch-test.cpp` and registered `asan_scratch_test` in CMake.
   - Verifies benign runs pass normally and `--trigger-crash` traps out-of-bounds reads under AddressSanitizer.

---

## Task 3: Current Behavior Summary (Pre-edit)

- **src/handlers/admin.cpp**: `handleGetUsers` calls `mapper.findAll()` returning users from all organizations; `handleListPermissions` filters by optional `organisation` query param, returning fleet-wide grants if omitted or allowing cross-tenant snooping if supplied; `handleGrantPermission` and `handleRevokePermission` do not verify that the target user or permission grant belongs to caller's session organisation; `handleAssignRole` and `handleRevokeRole` allow binding arbitrary users and roles across organisations.
- **src/handlers/storage-admin.cpp**: Storage overview, search, formats, analytics (`handleAnalyticsOverview`, `handleAnalyticsBreakdown`), and quota endpoints (`handleListQuotas`, `handleUpdateQuota`) query and mutate global fleet state without scoping to the caller's session organization; `handleGetSystemConfig`, `handleUpdateSystemConfig`, and `handlePurgeOrphans` are reachable by any user possessing `ADMIN` role.
- **src/services/admin.cpp**: `deserializeRegisterUserPayload` unconditionally consumes `organisation` from the request JSON, permitting org admins to register users into foreign organisations.
- **src/filters/admin.cpp**: `AdminFilter` validates only that `user_role_code == UserRoleEnum::ADMIN`, providing no distinction between global platform administrators and tenant organisation administrators.
- **DELETE /api/v1/admin/audit-logs**: Permitted for any organisation admin to purge audit logs for their own tenant without platform admin validation, and lacks an enforced minimum audit log retention period.

---

## Task 3: Completed Changes

1. **`docs/admin-route-audit.md` (New)**:
   - Exhaustively audited all 38 routes under `/api/v1/admin/*` across `AdminApiHandler` and `StorageAdminHandler`.
   - Documented pre-hardening caller, target organisation, and source. Zero "unscoped" rows remain post-hardening.
   - Formalized platform administrator resolution via `custom_config.security.system_admin_emails` and `SGRN_SYSTEM_ADMIN_EMAILS`.

2. **Platform Admin Helpers & Config (`include/sgrn/datastore/utils/helpers.hpp`, `configs/sgrn.json`, `configs/sgrn.dev.json`)**:
   - Implemented `isPlatformAdmin(tsp_req)` and `getSessionOrganisation(tsp_req)`.
   - Wired `security.system_admin_emails` into `configs/sgrn.json` and `configs/sgrn.dev.json` with fallback to `SGRN_SYSTEM_ADMIN_EMAILS` environment variable.
   - Updated `AdminFilter` to populate `is_platform_admin` request attribute.

3. **Core Admin Route Hardening (`src/handlers/admin.cpp`)**:
   - `handleGetUsers`: Replaced unbounded `mapper.findAll()` with SQL filter on session `organisation` (or requested org for platform admin). Rejects foreign `?organisation=` with 403 Forbidden.
   - `handleRegisterUser`: Non-platform admins forced to their session organisation; rejects foreign `organisation` in JSON payload with 403 Forbidden.
   - `handleListPermissions`: Forced non-platform admins to session organisation; rejects foreign `?organisation=` with 403 Forbidden.
   - `handleGrantPermission`: Verifies target user and requested organisation match caller's session organisation; rejects cross-tenant grants with 403 Forbidden.
   - `handleRevokePermission`: Enforces that the target permission grant ID / user belongs to caller's session organisation.
   - `handleAssignRole` & `handleRevokeRole`: Validates that role ID and target user ID / service ID belong to caller's session organisation before modification.
   - `handleGetMetaProbeSessions`: Filters active in-memory sessions by caller's session organisation for tenant admins.
   - `handlePurgeAuditLogs`: Gated strictly to platform admins (returns 403 Forbidden for tenant admins). Enforces a 90-day minimum retention floor (rejects `< 90` with 400 Bad Request) and logs a permanent `audit.purged` entry.

4. **Storage Admin Route Hardening (`src/handlers/storage-admin.cpp`)**:
   - `handleGetSystemConfig` & `handleUpdateSystemConfig`: Protected with platform admin check (returns 403 Forbidden for tenant admins).
   - `handlePurgeOrphans`: Gated to platform admins (returns 403 Forbidden for tenant admins).
   - `handleOverview`: Scopes DB objects and files census to caller's session organisation for tenant admins.
   - `handleSearch`: Scopes referencing files to caller's session organisation.
   - `handleListQuotas`: Enforces session organisation on user, service, and organisation queries for tenant admins.
   - `handleUpdateQuota`: Prevents tenant admins from updating foreign user/service quotas or foreign organisation caps (returns 403/404).
   - `handleAnalyticsOverview` & `handleAnalyticsBreakdown`: Scopes all breakdown slices and fleet statistics to caller's session organisation for tenant admins.

5. **Cross-Organisation Regression Test Suite (`tests/cross-org-auth-test.cpp`)**:
   - Unit and integration tests verifying platform vs tenant admin resolution, audit log purge 403/retention gating, system config gating, storage orphans purge gating, permissions cross-org query rejection, user registration cross-org rejection, and quota update cross-org rejection.
   - Registered `cross_org_auth_test` in `sgrn/lib/datastore/CMakeLists.txt`.



---

## Task 4: Current Behavior Summary (Pre-edit)

- **core.users**: Relies entirely on a legacy scalar `role` column (`admin` vs `user`), providing no database-native mechanism to designate platform-wide administrators (`is_platform_admin`).
- **core.domains**: Defines only a flat tuple `(id, organisation, name)` with a unique constraint on `(organisation, name)`. It lacks tree hierarchy pointers (`parent_id`), path tracking (`path`), and automated path reconciliation.
- **Administration Model**: Lacks any dedicated table or entity for scoped administrative privileges (`core.admin_assignments`). Administrative authority cannot be delegated at the domain level or partitioned across organizations.
- **core.admin_level**: No SQL-level function exists to evaluate a user's effective administrative tier (Platform = 3, Organisation = 2, Domain/Ancestor = 1, None = 0) against a target scope.
- **Subtree Integrity**: Domains can be modified or deleted without foreign-key parent-child restrictions preventing orphan domain subtrees or cyclical parent references.

---

## Task 4: Completed Changes

1. **Database Migration (`migrations/003_scoped_admins.sql`)**:
   - Idempotent migration adding `core.users.is_platform_admin` (default `false`).
   - Deprecated `core.users.role` with explicit documentation comment in favor of `core.admin_assignments` and `core.admin_level()`.
   - Domain Hierarchy: Added `parent_id` (FK with `ON DELETE RESTRICT`) and `path` to `core.domains` with unique constraint on `(organisation, path)`.
   - Domain Triggers: Added `trg_maintain_domain_path` trigger ensuring `path = parent.path || '/' || name` (or `'/' || name` for root domains) within the same organisation.
   - Subtree Relocation: Added `core.move_domain_subtree(p_domain_id, p_new_parent_id)` with cycle detection to rewrite subtree paths atomically.
   - Scoped Admin Assignments: Created `core.admin_assignments` with check constraint enforcing scope semantics (`domain` requires `domain_id`; `organisation` requires `domain_id IS NULL`) and unique index on `(user_id, scope, organisation, coalesce(domain_id, 0))`.
   - Authorization Function: Added `core.admin_level(user_id, org, domain_id)` returning 3 (Platform), 2 (Organisation), 1 (Domain or Ancestor Domain), or 0 (None) with exact path comparison (`left(path, length(x)+1) = x || '/'`).
   - Data Backfill: Migrated existing active users with `role = 'admin'` to organisation-level admin assignments, and marked the seed admin (`admin@local.com`) as `is_platform_admin = true`.

2. **Schema and View Updates (`postgres/schemas/core.sql`, `views/core/*.sql`, `crud/manifest.json`)**:
   - Synchronized `schemas/core.sql` and `seeding.sql` with new hierarchy columns, constraints, triggers, and functions.
   - Updated `views/core/domains.sql` to expose `parent_id` and `path`.
   - Updated `views/core/users.sql` to expose `is_platform_admin`.
   - Updated `crud/manifest.json` to expose `admin_assignments` resource and domain hierarchy columns (`parent_id`, `path`).

3. **Regression Test Suite (`tests/scoped-admins-test.cpp`)**:
   - Added unit test suite `scoped_admins_test` covering:
     * Idempotent migration execution (run twice).
     * Platform administrator resolution across multiple organizations.
     * Organisation administrator resolution and rejection for wrong organisation.
     * Hierarchical domain admin resolution (own subtree, descendant child, rejection for sibling, rejection for parent, rejection for foreign org).
     * Subtree relocation rewriting paths atomically and cycle detection rejection.
     * Domain deletion refusal for domains with children (`ON DELETE RESTRICT`).

4. **Build System & Isolated Codegen Integration**:
   - Upgraded `generate_orm.py` and `generate_views.py` with `--build-db` to dynamically provision an isolated build database (`sgrn_build_orm`) or fallback to an ephemeral Postgres cluster in `/tmp`.
   - Added `detectSuperuser` in `init-db.py`, `generate_orm.py`, and `generate_views.py` to auto-resolve OS-user superusers (`odahim`) and maintenance databases without hardcoded role dependencies.
   - Integrated dependency-tracked codegen into `cmake/sgrn_views_codegen.cmake`: Ninja/Make monitors `crud/manifest.json` and all `postgres/**/*.sql` files, automatically executing `generate_orm.py --build-db --views` whenever schemas or manifest change, zero-overhead when unchanged.
