# SGRN Datastore: Hardening Progress

## Task Status Table

| Task | Name                                         | Status                 | Branch              | Commit Range | Acceptance Evidence                                                                                                                                                                                                                                                           | Open Risks                       |
| ---- | -------------------------------------------- | ---------------------- | ------------------- | ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------- |
| 1    | Lock down Postgres, nginx, systemd           | ready for verification | datastore/hardening | working tree | `verify_test` unit tests added; configs & service files hardened                                                                                                                                                                                                              | Needs live systemd restart drill |
| 2    | Build hardening and sanitizer CI             | ready for verification | datastore/hardening | working tree | `build_hardening.cmake`, CI matrix (Release, ASan+UBSan, TSan), `asan-scratch-test.cpp`                                                                                                                                                                                       | Needs user build & sanitizer run |
| 3    | Fix cross-organisation leaks and audit purge | done                   | datastore/hardening | working tree | `docs/admin-route-audit.md` (38 routes, 0 unscoped), `cross_org_auth_test` (7/7 passed)                                                                                                                                                                                       | None                             |
| 12   | Fuzzing and regression suite (start early)   | todo                   | datastore/hardening | -            | -                                                                                                                                                                                                                                                                             | -                                |
| 4    | Schema for platform/org/domain admins        | done                   | datastore/hardening | working tree | `migrations/003_scoped_admins.sql`, `scoped_admins_test` (ALL CHECKS PASSED)                                                                                                                                                                                                  | None                             |
| 5    | ScopeFilter and route classification         | ready for verification | datastore/hardening | working tree | `docs/authz-matrix.md`, `authz_matrix_test` (>=200 cases evaluated), escalation guards                                                                                                                                                                                        | None                             |
| 6    | Tamper-evident audit log                     | done                   | datastore/hardening | working tree | `migrations/004_tamper_evident_audit_log.sql`, `audit_tamper_test` (6 test cases: sequential, tamper, delete, 8-writer concurrency, Ed25519 checkpoint, retention purge)                                                                                                      | None                             |
| 7    | Crypto core library                          | ready for verification | datastore/hardening | working tree | `sgrn/utils/crypto/*` (`Dek`, `KeyProvider`, `aead`, `stream`), `crypto_test` (8 test suites: KAT vector, DEK zeroize, key lifecycle, boundary round-trips, tamper resistance, nonce uniqueness, range decryption, zero key leak)                                             | None                             |
| 8    | Encrypted storage & SQLite key store         | ready for verification | datastore/hardening | working tree | `migrations/005_encrypted_storage.sql`, `SqliteKeyProvider` (0700/0600 mode, path isolation), AEAD stream upload/download/range, presigned URL block, crypto-shred on delete, `--backup-keys`, `--verify-keys`, `encryption_test` (7 test cases passed), `docs/encryption.md` | None                             |
| 9    | Key lifecycle, crypto-shred, backups         | todo                   | datastore/hardening | -            | -                                                                                                                                                                                                                                                                             | -                                |
| 10   | Authentication and sessions                  | todo                   | datastore/hardening | -            | -                                                                                                                                                                                                                                                                             | -                                |
| 11   | Editing, versions and text deltas            | todo                   | datastore/hardening | -            | -                                                                                                                                                                                                                                                                             | -                                |
| 13   | Threat model and operations evidence         | todo                   | datastore/hardening | -            | -                                                                                                                                                                                                                                                                             | -                                |

---

## Task 1: Current Behavior Summary (Pre-edit)

- **configs/postgres/pg_hba.conf**: Permits local Unix domain socket and IPv4 loopback (127.0.0.1/32) authentication using insecure `trust`, alongside a wide-open `host all all 0.0.0.0/0` rule using SCRAM-SHA-256 for all databases and users.
- **configs/postgres/postgresql.conf**: Binds PostgreSQL to all network interfaces (`listen_addresses = '*'`), leaves SSL disabled (`#ssl = off`), TLS min protocol commented out at TLSv1.2, and connection/disconnection logging disabled.
- **configs/nginx/sites-enabled/sgrn.conf & nginx.conf**: TLS configuration allows TLSv1.2 with default broad ciphers (`HIGH:!aNULL:!MD5`), lacks HSTS, X-Content-Type-Options, Referrer-Policy, and strict CSP headers, and sets `client_max_body_size 0` in `/minio/`.
- **configs/systemd/\*.service**: Service units (`SGRN-datastore`, `SGRN-garage`, `SGRN-postgres`, `SGRN-nginx`, `sgrn`) run without seccomp system call filtering (`SystemCallFilter`), without dropped capabilities (`CapabilityBoundingSet`), and with relaxed filesystem isolation (`ProtectSystem=full` or unset, `ProtectHome=false`).
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
     - Idempotent migration execution (run twice).
     - Platform administrator resolution across multiple organizations.
     - Organisation administrator resolution and rejection for wrong organisation.
     - Hierarchical domain admin resolution (own subtree, descendant child, rejection for sibling, rejection for parent, rejection for foreign org).
     - Subtree relocation rewriting paths atomically and cycle detection rejection.
     - Domain deletion refusal for domains with children (`ON DELETE RESTRICT`).

4. **Build System & Isolated Codegen Integration**:
   - Upgraded `generate_orm.py` and `generate_views.py` with `--build-db` to dynamically provision an isolated build database (`sgrn_build_orm`) or fallback to an ephemeral Postgres cluster in `/tmp`.
   - Added `detectSuperuser` in `init-db.py`, `generate_orm.py`, and `generate_views.py` to auto-resolve OS-user superusers (`odahim`) and maintenance databases without hardcoded role dependencies.
   - Integrated dependency-tracked codegen into `cmake/sgrn_views_codegen.cmake`: Ninja/Make monitors `crud/manifest.json` and all `postgres/**/*.sql` files, automatically executing `generate_orm.py --build-db --views` whenever schemas or manifest change, zero-overhead when unchanged.

---

## Task 5: Completed Changes

1. **AdminLevel & TargetResource Types (`filters/types.hpp`)**:
   - Added `AdminLevel` enum: `NONE = 0`, `DOMAIN = 1`, `ORG = 2`, `PLATFORM = 3`.
   - Added `TargetResource` struct with `organisation` and `optional<int32_t> domain_id`.
   - Added string conversion helpers `adminLevelToString()` and `adminLevelFromString()`.

2. **ScopeFilter & AdminLevelCache (`filters/admin.hpp`, `filters/admin.cpp`)**:
   - Implemented `AdminLevelCache`: In-process TTL cache over `core.admin_level()`, with fail-closed semantics (`NONE` on DB failure/disconnect).
   - Implemented `ScopeFilter`: Enforces tenant boundaries (blocks foreign `?organisation=` and cross-tenant JSON bodies for non-platform users), hydrates request attributes (`admin_level`, `is_platform_admin`, `target_organisation`), and checks level hierarchy.
   - Provided concrete filter classes `PlatformAdminFilter`, `OrgAdminFilter`, and `DomainAdminFilter`.
   - Wired `requireLevel(level, resolver)` filter factory.

3. **Session Layer Invalidation & Hydration (`src/session/SessionStore.cpp`, `src/filters/auth.cpp`, `src/services/auth.cpp`)**:
   - Integrated `AdminLevelCache::instance().invalidateUser(uid)` into the PostgreSQL LISTEN/NOTIFY loop on session `revoke` and `refresh` actions.
   - Hydrated session JSON and user details at login and session resolution with authoritative server-computed `is_platform_admin`, `admin_level`, and `admin_level_name`.

4. **Route Reclassification & Authorization Matrix (`handlers/admin.hpp`, `handlers/storage-admin.hpp`, `docs/authz-matrix.md`)**:
   - Classified and registered all `/api/v1/admin/*` routes with explicit `PlatformAdminFilter` or `OrgAdminFilter`.
   - Destructive operations (`DELETE /admin/audit-logs`, `POST /admin/storage/orphans/purge`, `GET/PUT /admin/system/config`) gated strictly to `PlatformAdminFilter`.
   - Published full route authorization classification table in `docs/authz-matrix.md`.
   - Added `operation_filters` to `crud/manifest.json` for `admin_assignments` to restrict all CRUD operations to `OrgAdminFilter`.

5. **Privilege Escalation Guards (`handlers/admin.cpp`)**:
   - **Self-grant protection**: Prohibited admins from granting permissions or assigning roles to themselves (`user_id == caller_id`), returning 403 Forbidden and auditing the attempt.
   - **Above-level grant protection**: Prohibited non-platform admins from assigning system roles, granting platform admin status, or registering users with `is_platform_admin = true` or `role = 'platform_admin'`.
   - **Last platform admin protection**: Prevented revoking roles or admin status from the last active platform administrator.
   - **Auditing**: All elevation attempts and permission grants audited via `AuditLogger`.

6. **Frontend UI Scoping (`sgrn/web/datastore/src/pages/signin/types.ts`, `sgrn/web/datastore/src/pages/admin/page.tsx`)**:
   - Extended `User` interface with `admin_level`, `admin_level_name`, and `is_platform_admin`.
   - Conditionally hid Platform-only UI controls (System Config navigation tab, Garage Storage orphan purge buttons) when caller is not a platform administrator.

7. **Authorization Matrix Regression Test (`tests/authz-matrix-test.cpp`)**:
   - Implemented unit test suite `authz_matrix_test` evaluating 256 matrix cells (8 caller types × 32 routes) plus cross-tenant tampering checks, totaling > 200 cases.
   - Tested privilege escalation defenses: self-grant, self-assign, above-level registration, cross-tenant registration, and cache invalidation hooks.
   - Registered `authz_matrix_test` in `CMakeLists.txt`.

---

## Task 6: Completed Changes

1. **Schema & Migration (`migrations/004_tamper_evident_audit_log.sql`, `postgres/schemas/audit_logs.sql`)**:
   - Added monotonic `seq`, cryptographic `prev_hash`, and canonical `row_hash = SHA-256(prev_hash || canonical row)` columns to `core.audit_logs`.
   - Created `core.audit_checkpoints` table for Ed25519-signed checkpoints (`organisation`, `last_seq`, `last_hash`, `signature`, `public_key`).
   - Implemented `core.append_audit_log()` `SECURITY DEFINER` function using per-organisation advisory transaction locks (`pg_advisory_xact_lock`) ensuring serialized sequence numbering and chain integrity under high concurrency.
   - Implemented `core.verify_audit_chain()` SQL function checking sequence continuity, prev_hash chaining, and SHA-256 row hash verification.
   - Installed `trg_audit_tamper_guard` trigger prohibiting direct `UPDATE` or `DELETE` on `core.audit_logs`.

2. **Audit Logging Integration (`include/sgrn/datastore/audit/AuditLogger.hpp`, `src/audit/AuditLogger.cpp`)**:
   - Updated `AuditLogger::log()` to use `core.append_audit_log()`.
   - Added `AuditLogger::verifyChain()` and `AuditLogger::createCheckpoint()`.
   - Wired critical audit hooks across services: `auth.login`, `auth.failed`, `session.revoked`, `system.config_updated`, `audit.purged`, and role/permission changes.
   - Added route `GET /api/v1/admin/audit-logs/verify` and CLI `--verify-audit [org]`.

3. **Key Generators & Crypto Utilities (`sgrn/utils`)**:
   - Created `include/sgrn/utils/crypto.hpp` and `include/sgrn/utils/keygen.hpp` in `sgrn_utils`.
   - Implemented Ed25519 key pair generation, raw signing, and verification.
   - Implemented cryptographically secure random bytes and hex key generation.
   - Delegated datastore audit signing routines to `sgrn::utils::crypto`.

4. **Retention Purge & Chain Continuity (`core.purge_audit_logs`)**:
   - Implemented `core.purge_audit_logs(p_organisation, p_older_than_interval)` `SECURITY DEFINER` routine enforcing >= 90 days retention floor.
   - Purges only verified segments up to the most recent signed checkpoint (`seq <= v_checkpoint.last_seq`).
   - Trigger `trg_audit_tamper_guard` permits `DELETE` only when session config `sgrn.allow_audit_purge = 'on'`, preventing uncoordinated row deletions.
   - Enhanced `core.verify_audit_chain()` to seamlessly verify chains where `min(seq) > 1` against the corresponding signed checkpoint, while catching unauthorized gap deletions.
   - Integrated into `handlePurgeAuditLogs` in `src/handlers/admin.cpp` with immutable `audit.purged` logging.

5. **Tamper, Concurrency & Retention Test Suite (`tests/audit-tamper-test.cpp`)**:
   - Sequential append and chain verification (seq 1..10).
   - SQL tamper detection (direct superuser UPDATE on row 5 causes verify failure at seq 5).
   - SQL delete detection (superuser DELETE of row 3 causes verify failure at seq 4).
   - 8-writer concurrency load test (400 concurrent writes serialized without gaps or duplicate seqs).
   - Ed25519 checkpoint signing and verification (validates signature; rejects corrupted signature or tampered hash).
   - Retention purge of verified segments (direct DELETE blocked by trigger; purge up to signed checkpoint leaves remaining chain verifiable; uncheckpointed gap caught).

---

## Task 7: Completed Changes

1. **DEK Type (`include/sgrn/utils/crypto/dek.hpp`, `src/crypto/dek.cpp`)**:
   - Implemented `Dek` as a move-only 32-byte cryptographic key with `OPENSSL_cleanse` zeroization on destruction and move.
   - Prohibited stream formatting (`operator<<` deleted) and disabled `fmt::formatter` specialization to eliminate key leak risks.
   - Constant-time equality comparison via `CRYPTO_memcmp`.
   - Factory methods: `Dek::generate()` from OpenSSL CSPRNG and `Dek::fromBytes()`.

2. **KeyProvider & KeyRecord Architecture (`include/sgrn/utils/crypto/key-provider.hpp`, `src/crypto/key-provider.cpp`)**:
   - Abstract `KeyProvider` interface: `createPending()`, `activate(key_id, file_id)`, `get(key_id)`, `shred(key_id)`, and `listForReconcile()`.
   - `KeyRecord` struct with `wrap_scheme` field (e.g. `NONE`, `AES_KW`, `KMS`) enabling future KMS wrapping providers without breaking callers.
   - Thread-safe `InMemoryKeyProvider` providing complete key lifecycle and permanent crypto-shredding (zeroizes key in memory, marks `SHREDDED`, blocks subsequent access or activation).

3. **Chunked AEAD Stream Primitives (`include/sgrn/utils/crypto/aead.hpp`, `src/crypto/aead.cpp`)**:
   - AES-256-GCM authenticated encryption using OpenSSL EVP cipher context APIs.
   - Nonce construction: 64-bit CSPRNG per-file random prefix + 32-bit big-endian monotonic chunk counter (12 bytes total), strictly preventing nonce reuse under any single DEK.
   - Canonical AAD: `key_id || chunk_index (4B BE) || flags (1B) || total_plaintext_size (8B BE)`.
   - Final chunk marker flag (`kChunkFlagFinal = 0x01`) bound into AAD, preventing stream truncation and unauthorized trailing data.

4. **Streaming & Random-Access Range Decryption (`include/sgrn/utils/crypto/stream.hpp`, `src/crypto/stream.cpp`)**:
   - `encryptBuffer()` and `decryptBuffer()` for full in-memory chunked AEAD operations.
   - `encryptStream()` and `decryptStream()` streaming APIs reading and writing `std::istream` / `std::ostream` in configurable chunk sizes (default 1 MiB).
   - `decryptRange()`: $O(1)$ chunk seek and range extraction enabling random-access HTTP Range requests without reading or decrypting unrelated chunks.

5. **Crypto Test Suite (`sgrn/lib/utils/tests/crypto-test.cpp`)**:
   - NIST SP 800-38D AES-256-GCM known-answer test vector verification.
   - DEK move-only semantics, zeroization, and constant-time equality checks.
   - KeyProvider lifecycle: `createPending`, `activate`, `get`, `shred`, `listForReconcile`.
   - Boundary size round-trips: 0 bytes (empty file), 1 byte, $N-1$, $N$ (exact chunk boundary), $N+1$, multi-chunk buffers & streams.
   - Tamper resistance: bit flips in payload, corrupted header magic, truncation, unexpected trailing data, wrong DEK, wrong key_id in AAD, chunk swapping (chunk 0 vs chunk 1) all failing with distinct integrity errors.
   - Nonce uniqueness: 1000 CSPRNG prefixes and 1000 sequential counters confirmed strictly collision-free.
   - Random-access range queries: sub-ranges across chunk boundaries validated against plaintext slices.
   - Grep security audit: verified zero key bytes or hex representations leak into logs or error messages.
   - Target `crypto_test` registered in `sgrn/lib/utils/CMakeLists.txt`.

---

## Task 8: Completed Changes

1. **SQLite Key Provider (`include/sgrn/datastore/crypto/sqlite-key-provider.hpp`, `src/crypto/sqlite-key-provider.cpp`)**:
   - Implemented `SqliteKeyProvider`: the sole component reading or writing `$SGRN_DATA_DIR/keys/keys.db`.
   - Permissions check: directory strictly `0700`, database file strictly `0600`; fails startup otherwise.
   - Path isolation: verified outside Garage and PostgreSQL data directories.
   - PRAGMAs: `journal_mode=WAL`, `synchronous=FULL`, `secure_delete=ON`, `foreign_keys=ON`, `busy_timeout=5000`.
   - `createPending(object_key)`: commits and fsyncs pending key row via `PRAGMA wal_checkpoint(FULL)` before ciphertext is stored.
   - `activate(key_id, file_id)`: binds key to confirmed PostgreSQL file ID.
   - `get(key_id)`: fails closed on unknown wrap schemes; zero key material in errors/logs.
   - `shred(key_id)`: securely overwrites DEK blob with zeros (`OPENSSL_cleanse`), sets status `SHREDDED`, checkpoints.
   - Global singleton manager: `key-store.hpp` / `key-store.cpp` initialized at startup in `init.hpp`.

2. **PostgreSQL Migration 005 (`migrations/005_encrypted_storage.sql`, `postgres/schemas/`)**:
   - `core.domains.encryption_policy` enum (`'none'`, `'optional'`, `'required'`).
   - `storage.files.key_id` (text, default null) and `storage.files.is_encrypted` (boolean, default false).
   - Partial unique index `uq_files_key_id` on `storage.files (key_id)` where `key_id IS NOT NULL`.
   - Check constraint `chk_files_encryption_consistency`: enforces `(is_encrypted = false AND key_id IS NULL) OR (is_encrypted = true AND key_id IS NOT NULL)`.
   - Added `is_encrypted` and `pending_key_id` to `storage.upload_sessions` for cleanup tracking.

3. **Upload & Download Pipeline Integration**:
   - `handleInitUploadSession`: evaluates target domain's `encryption_policy`. Enforces encryption for `required`, blocks for `none`, respects client choice for `optional`.
   - `handleCompleteUploadSession`: on encrypted sessions, generates DEK and stores pending key, encrypts payload via AEAD stream (`encryptBuffer`), writes ciphertext to Garage under `uploads/<hash>/0`, bypasses object deduplication, commits Postgres file record with `key_id`, activates key in SQLite store. On error, shreds key and purges partial ciphertext.
   - `handleDownloadFileRequest`: detects `record.is_encrypted`, fetches DEK via `getKeyStore()->get(key_id)`, decrypts on-the-fly. For HTTP Range requests, delegates to `decryptRange()` for chunk-aligned $O(1)$ seek and slice decryption.
   - Blocked presigned URLs: `handlePresignedUrl` in `storage-drive.cpp` checks `is_encrypted` and returns 403 Forbidden ("Presigned URLs are not supported for encrypted files").
   - File deletion / crypto-shred: all file deletion paths (`StorageService::deleteFile`, `handleDeleteObject`, `StorageApiHandler::deleteFile`) query `is_encrypted` and `key_id` and invoke `key_store->shred(key_id)` before deleting rows.

4. **CLI Key Management (`--backup-keys`, `--verify-keys`)**:
   - `--backup-keys`: uses SQLite online backup API (`sqlite3_backup`) to snapshot `keys.db` with mode `0600`. Emits mandatory prominent warning regarding physical separation of key and data backups.
   - `--verify-keys`: reconciles every encrypted file in PostgreSQL with `keys.db`, detecting missing keys, orphaned active keys, and abandoned upload keys.
   - Integrated into `--verify-install` in `verify.cpp` to validate mode `0700`/`0600` and path isolation.

5. **Documentation & Tests**:
   - Created `docs/encryption.md` with explicit demo configuration disclaimer (server process and root can read keys; platform admins are not cryptographically separated from domain data; future KMS upgrade path via `wrap_scheme`).
   - Created `sgrn/lib/datastore/tests/encryption-test.cpp` (7 test cases: provider lifecycle, stream & range decryption, tamper resistance, Postgres schema constraints, reconcile CLI, online backup). Registered `encryption_test` in CMake.
