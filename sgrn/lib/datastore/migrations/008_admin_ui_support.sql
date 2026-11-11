-- SGRN migration 008: admin UI support (WP2)
--
-- Schema + function support for the admin console overhaul:
--   1. core.domains: per-domain capacity caps (storage_limit,
--      entry_count_limit) — OCI-style namespace quotas.
--   2. Tenant consistency: core.trg_assert_same_org rejects admin_assignments
--      that point their domain_id into a different organisation (cross-tenant
--      escalation route pre-existing in core.admin_level()).
--   3. Lifecycle session revocation: suspending / soft-deleting a user or
--      service (or suspending / archiving an organisation) now terminates its
--      live sessions at the DB layer (trg_revoke_*_sessions).  Previously
--      only the *sign-in* path was gated and issued tokens outlived the
--      account's active state.
--   4. Hardened authenticate_user / authenticate_automated_service (status,
--      soft-delete and organisation-status predicates).
--   5. storage.domain_usage + storage.enforce_domain_quota: domain_id is now
--      derived from the domain label on every storage write and the domain's
--      entry/byte caps are enforced (they were never enforced because nothing
--      ever wrote domain_id and the label alone was not tenant-qualified).
--   6. Byte-level quota enforcement re-pointed (F11): a BEFORE INSERT trigger
--      on storage.files cannot see the file's file_objects mappings (they
--      are inserted later), which made v_original_size always 0 — actor and
--      org byte caps silently never enforced, and actor byte totals never
--      advanced (+1 entry, +0 bytes per upload).  Byte accounting/limits now
--      evaluated at the file_objects stage (storage.apply_object_owner_usage)
--      with recompute-from-live-mappings semantics; enforce_storage_limit
--      keeps the entry-count checks only.
--   7. Org aggregates include domain-namespace storage (previously invisible:
--      domain rows carry no actor) via core.sync_org_totals_for.
--   8. core.rename_domain (atomic rename incl. storage namespace text) and
--      core.create_organisation (validated creation entry point).
--
-- CONVENTIONS: idempotent (safe to run repeatedly).  Applies on top of
-- migration 007.  Pre-existing cross-org admin assignments and unresolvable
-- domain labels are REPORTED (NOTICE), not modified — resolve them manually;
-- new writes are guarded from the moment the migration lands.
--
-- Mirrors: every statement here also lives in postgres/schemas/*.sql and
-- postgres/functions/*.sql for fresh installs.

BEGIN;

-- ============================================================
-- 1. core.domains quota columns
-- ============================================================
ALTER TABLE core.domains
  ADD COLUMN IF NOT EXISTS storage_limit bigint DEFAULT NULL;

ALTER TABLE core.domains
  ADD COLUMN IF NOT EXISTS entry_count_limit bigint DEFAULT NULL;

DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'chk_domains_storage_limit_nonneg') THEN
    ALTER TABLE core.domains ADD CONSTRAINT chk_domains_storage_limit_nonneg
      CHECK (storage_limit IS NULL OR storage_limit >= 0);
  END IF;

  IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'chk_domains_entry_count_limit_nonneg') THEN
    ALTER TABLE core.domains ADD CONSTRAINT chk_domains_entry_count_limit_nonneg
      CHECK (entry_count_limit IS NULL OR entry_count_limit >= 0);
  END IF;
END
$$;

-- ============================================================
-- 2. Tenant consistency guard for admin assignments
-- ============================================================
-- Report pre-existing cross-org assignments instead of failing the
-- migration; the operator should clean them up (they are landmines for
-- core.admin_level()); the NEW trigger blocks further cross-org writes.
DO $$
DECLARE
  v_bad bigint;
BEGIN
  SELECT count(*) INTO v_bad
  FROM core.admin_assignments a
  JOIN core.users u ON u.id = a.user_id
  WHERE u.organisation <> a.organisation;

  IF v_bad > 0 THEN
    RAISE NOTICE
      'migration 008: % admin_assignments row(s) name a user of a DIFFERENT organisation — not modified, clean up manually',
      v_bad;
  END IF;

  SELECT count(*) INTO v_bad
  FROM core.admin_assignments a
  JOIN core.domains d ON d.id = a.domain_id
  WHERE a.domain_id IS NOT NULL AND d.organisation <> a.organisation;

  IF v_bad > 0 THEN
    RAISE NOTICE
      'migration 008: % admin assignment(s) reference a domain of ANOTHER organisation — not modified, clean up manually (they are cross-tenant escalation routes)',
      v_bad;
  END IF;

  SELECT count(*) INTO v_bad
  FROM core.user_domain_permissions p
  JOIN core.users u ON u.id = p.user_id
  WHERE u.organisation <> p.organisation;

  IF v_bad > 0 THEN
    RAISE NOTICE
      'migration 008: % user_domain_permissions row(s) name a user of a DIFFERENT organisation — not modified, clean up manually',
      v_bad;
  END IF;
END
$$;

CREATE OR REPLACE FUNCTION core.trg_assert_same_org () RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE
  v_domain_org text;
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM core.users u
    WHERE u.id = NEW.user_id AND u.organisation = NEW.organisation
  ) THEN
    RAISE EXCEPTION 'User % does not belong to organisation %', NEW.user_id, NEW.organisation
      USING errcode = '23514';
  END IF;

  IF TG_TABLE_NAME = 'admin_assignments' AND NEW.domain_id IS NOT NULL THEN
    SELECT organisation INTO v_domain_org FROM core.domains WHERE id = NEW.domain_id;

    IF NOT FOUND THEN
      RAISE EXCEPTION 'Admin assignment references missing domain %', NEW.domain_id
        USING errcode = '23503';
    END IF;

    IF v_domain_org IS DISTINCT FROM NEW.organisation THEN
      RAISE EXCEPTION
        'Cross-organisation admin assignment rejected: domain % belongs to %, assignment targets %',
        NEW.domain_id, v_domain_org, NEW.organisation
        USING errcode = '23514';
    END IF;
  END IF;

  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS trg_assert_same_org ON core.admin_assignments;
CREATE TRIGGER trg_assert_same_org
BEFORE INSERT OR UPDATE OF user_id, organisation, domain_id ON core.admin_assignments
FOR EACH ROW EXECUTE FUNCTION core.trg_assert_same_org ();

DROP TRIGGER IF EXISTS trg_assert_same_org ON core.user_domain_permissions;
CREATE TRIGGER trg_assert_same_org
BEFORE INSERT OR UPDATE OF user_id, organisation ON core.user_domain_permissions
FOR EACH ROW EXECUTE FUNCTION core.trg_assert_same_org ();

-- ============================================================
-- 3. Lifecycle-driven session revocation
-- ============================================================
-- Terminating a row writes terminated_at/termination_reason, which fires the
-- existing trg_enqueue_session_revoke → every instance's notify loop
-- invalidates the token.  Reactivation does not resurrect sessions.

CREATE OR REPLACE FUNCTION core.trg_revoke_user_sessions () RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.status IS DISTINCT FROM 'active'
     OR NEW.is_active = false
     OR NEW.deleted_at IS NOT NULL THEN
    UPDATE core.sessions
    SET terminated_at = now(),
        termination_reason = 'revoked'
    WHERE user_id = NEW.id
      AND terminated_at IS NULL;
  END IF;
  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS trg_revoke_user_sessions ON core.users;
CREATE TRIGGER trg_revoke_user_sessions
AFTER UPDATE OF status, is_active, deleted_at ON core.users
FOR EACH ROW EXECUTE FUNCTION core.trg_revoke_user_sessions ();

CREATE OR REPLACE FUNCTION core.trg_revoke_service_sessions () RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.deleted_at IS NOT NULL THEN
    UPDATE core.sessions
    SET terminated_at = now(),
        termination_reason = 'revoked'
    WHERE automated_service_id = NEW.id
      AND terminated_at IS NULL;
  END IF;
  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS trg_revoke_service_sessions ON core.automated_services;
CREATE TRIGGER trg_revoke_service_sessions
AFTER UPDATE OF status, deleted_at ON core.automated_services
FOR EACH ROW EXECUTE FUNCTION core.trg_revoke_service_sessions ();

CREATE OR REPLACE FUNCTION core.trg_revoke_org_sessions () RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.status IS DISTINCT FROM 'active' THEN
    UPDATE core.sessions s
    SET terminated_at = now(),
        termination_reason = 'revoked'
    WHERE s.terminated_at IS NULL
      AND (
        s.user_id IN (SELECT u.id FROM core.users u WHERE u.organisation = NEW.name)
        OR s.automated_service_id IN (SELECT a.id FROM core.automated_services a WHERE a.organisation = NEW.name)
      );
  END IF;
  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS trg_revoke_org_sessions ON core.organisations;
CREATE TRIGGER trg_revoke_org_sessions
AFTER UPDATE OF status ON core.organisations
FOR EACH ROW EXECUTE FUNCTION core.trg_revoke_org_sessions ();

-- ============================================================
-- 4. Hardened authentication functions
-- ============================================================
CREATE OR REPLACE FUNCTION core.authenticate_user (t_email text, t_password text) RETURNS SETOF core.user_details
LANGUAGE sql STABLE SECURITY DEFINER
SET search_path = core, public
AS $$
    SELECT ud.*
    FROM   core.user_details ud
    JOIN   core.users        u  ON u.id = ud.id
    WHERE  u.email     = t_email
      AND  u.is_active = true
      AND  u.status    = 'active'
      AND  u.deleted_at IS NULL
      AND  EXISTS (SELECT 1 FROM core.organisations o
                   WHERE o.name = u.organisation AND o.status = 'active')
      AND  u.password  = crypt(t_password, u.password);
$$;

REVOKE ALL ON FUNCTION core.authenticate_user (text, text) FROM public;
GRANT EXECUTE ON FUNCTION core.authenticate_user (text, text) TO sgrn_datastore;

CREATE OR REPLACE FUNCTION core.authenticate_automated_service (t_token uuid, t_secret text) RETURNS TABLE (
  id int,
  organisation text,
  name text,
  token uuid,
  status text,
  metadata jsonb,
  total_virtual_size bigint,
  total_real_size bigint,
  storage_limit bigint,
  total_entry_count bigint,
  entry_count_limit bigint,
  created_at timestamptz,
  updated_at timestamptz,
  domain text
) LANGUAGE sql STABLE SECURITY DEFINER
SET search_path = core, public AS $$
    SELECT
        a.id,
        a.organisation,
        a.name,
        a.token,
        a.status,
        a.metadata,
        a.total_virtual_size,
        a.total_real_size,
        a.storage_limit,
        a.total_entry_count,
        a.entry_count_limit,
        a.created_at,
        a.updated_at,
        a.domain
    FROM core.automated_services a
    WHERE a.token = t_token
      AND a.token_secret_hash = crypt(t_secret, a.token_secret_hash)
      AND a.deleted_at IS NULL
      AND EXISTS (
        SELECT 1 FROM core.organisations o
        WHERE o.name = a.organisation AND o.status = 'active'
      );
$$;

REVOKE ALL ON FUNCTION core.authenticate_automated_service (uuid, text) FROM public;
GRANT EXECUTE ON FUNCTION core.authenticate_automated_service (uuid, text) TO sgrn_datastore;

-- ============================================================
-- 5. Org aggregate functions (split for reuse; domain rows now counted)
-- ============================================================
CREATE OR REPLACE FUNCTION core.sync_org_totals_for (p_org text) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
  UPDATE core.organisations
  SET
    total_virtual_size = (
      SELECT coalesce(sum(u.total_virtual_size), 0)
      FROM core.users u
      WHERE u.organisation = p_org AND u.deleted_at IS NULL
    ) + (
      SELECT coalesce(sum(a.total_virtual_size), 0)
      FROM core.automated_services a
      WHERE a.organisation = p_org AND a.deleted_at IS NULL
    ) + (
      SELECT coalesce(sum(o.original_size), 0)
      FROM storage.files f
      JOIN storage.file_objects fo ON fo.file_id = f.id
      JOIN storage.objects o ON o.id = fo.object_id
      WHERE f.user_id IS NULL
        AND f.automated_service_id IS NULL
        AND f.domain_id IN (SELECT d.id FROM core.domains d WHERE d.organisation = p_org)
    ),
    total_real_size = (
      SELECT coalesce(sum(u.total_real_size), 0)
      FROM core.users u
      WHERE u.organisation = p_org AND u.deleted_at IS NULL
    ) + (
      SELECT coalesce(sum(a.total_real_size), 0)
      FROM core.automated_services a
      WHERE a.organisation = p_org AND a.deleted_at IS NULL
    ) + (
      SELECT coalesce(sum(o.size), 0)
      FROM storage.files f
      JOIN storage.file_objects fo ON fo.file_id = f.id
      JOIN storage.objects o ON o.id = fo.object_id
      WHERE f.user_id IS NULL
        AND f.automated_service_id IS NULL
        AND f.domain_id IN (SELECT d.id FROM core.domains d WHERE d.organisation = p_org)
    ),
    total_entry_count = (
      SELECT coalesce(sum(u.total_entry_count), 0)
      FROM core.users u
      WHERE u.organisation = p_org AND u.deleted_at IS NULL
    ) + (
      SELECT coalesce(sum(a.total_entry_count), 0)
      FROM core.automated_services a
      WHERE a.organisation = p_org AND a.deleted_at IS NULL
    ) + (
      SELECT count(*) FROM storage.files f
      WHERE f.user_id IS NULL
        AND f.automated_service_id IS NULL
        AND f.domain_id IN (SELECT d.id FROM core.domains d WHERE d.organisation = p_org)
    ) + (
      SELECT count(*) FROM storage.directories d
      WHERE d.user_id IS NULL
        AND d.automated_service_id IS NULL
        AND d.domain_id IN (SELECT d2.id FROM core.domains d2 WHERE d2.organisation = p_org)
    )
  WHERE name = p_org;
END;
$$;

CREATE OR REPLACE FUNCTION core.sync_org_storage_totals () RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE
  v_org text;
BEGIN
  v_org := coalesce(NEW.organisation, OLD.organisation);

  PERFORM core.sync_org_totals_for(v_org);

  IF TG_OP = 'DELETE' THEN
    RETURN OLD;
  END IF;
  RETURN NEW;
END;
$$;

-- ============================================================
-- 6. Domain namespace: usage + quota derivation/enforcement
-- ============================================================
CREATE OR REPLACE FUNCTION storage.domain_usage (p_domain_id int)
RETURNS TABLE (virtual_size bigint, real_size bigint, entry_count bigint)
LANGUAGE sql STABLE AS $$
  SELECT
    coalesce((
      SELECT sum(o.original_size)
      FROM storage.files f
      JOIN storage.file_objects fo ON fo.file_id = f.id
      JOIN storage.objects o ON o.id = fo.object_id
      WHERE f.domain_id = p_domain_id
    ), 0)::bigint,
    coalesce((
      SELECT sum(o.size)
      FROM storage.files f
      JOIN storage.file_objects fo ON fo.file_id = f.id
      JOIN storage.objects o ON o.id = fo.object_id
      WHERE f.domain_id = p_domain_id
    ), 0)::bigint,
    (SELECT count(*)::bigint FROM storage.files f WHERE f.domain_id = p_domain_id)
      + (SELECT count(*)::bigint FROM storage.directories d WHERE d.domain_id = p_domain_id)
$$;

CREATE OR REPLACE FUNCTION storage.enforce_domain_quota () RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE
  v_org          text;
  v_domain_id    int;
  v_domain_name  text;
  v_entry_limit      bigint;
  v_entry_count      bigint;
  v_org_entry_limit  bigint;
  v_org_entry_count  bigint;
BEGIN
  IF NEW.domain_id IS NOT NULL THEN
    SELECT organisation, name INTO v_org, v_domain_name FROM core.domains WHERE id = NEW.domain_id;

    IF v_org IS NULL THEN
      RAISE EXCEPTION 'Domain % does not exist', NEW.domain_id USING errcode = '23503';
    END IF;

    IF NEW.domain IS NOT NULL AND NEW.domain IS DISTINCT FROM v_domain_name THEN
      RAISE EXCEPTION 'Domain label % does not match domain id %', NEW.domain, NEW.domain_id
        USING errcode = '23514';
    END IF;
    NEW.domain := v_domain_name;
  ELSIF NEW.domain IS NOT NULL THEN
    IF NEW.user_id IS NOT NULL THEN
      SELECT organisation INTO v_org FROM core.users WHERE id = NEW.user_id;
    ELSIF NEW.automated_service_id IS NOT NULL THEN
      SELECT organisation INTO v_org FROM core.automated_services WHERE id = NEW.automated_service_id;
    ELSE
      SELECT coalesce(u.organisation, a.organisation) INTO v_org
      FROM core.sessions s
      LEFT JOIN core.users u ON u.id = s.user_id
      LEFT JOIN core.automated_services a ON a.id = s.automated_service_id
      WHERE s.id = NEW.session_id;
    END IF;

    IF v_org IS NULL THEN
      RAISE EXCEPTION 'Domain-scoped row requires a resolvable owner (session %)', NEW.session_id
        USING errcode = '23503';
    END IF;

    SELECT d.id INTO v_domain_id
    FROM core.domains d
    WHERE d.organisation = v_org AND d.name = NEW.domain;

    IF v_domain_id IS NULL THEN
      RAISE EXCEPTION 'Unknown domain % for organisation %', NEW.domain, v_org
        USING errcode = '23503';
    END IF;
    NEW.domain_id := v_domain_id;
  END IF;

  IF TG_TABLE_NAME = 'files' AND NEW.domain_id IS NOT NULL THEN
    SELECT entry_count_limit INTO v_entry_limit FROM core.domains WHERE id = NEW.domain_id;

    IF v_entry_limit IS NOT NULL THEN
      v_entry_count := (SELECT count(*) FROM storage.files f WHERE f.domain_id = NEW.domain_id)
                     + (SELECT count(*) FROM storage.directories d WHERE d.domain_id = NEW.domain_id);

      IF v_entry_count + 1 > v_entry_limit THEN
        RAISE EXCEPTION 'Domain entry count limit exceeded'
          USING errcode = '55P03',
                detail  = format('Domain %s entry limit: %s, Current entries: %s',
                                 NEW.domain, v_entry_limit, v_entry_count);
      END IF;
    END IF;

    SELECT entry_count_limit INTO v_org_entry_limit FROM core.organisations WHERE name = v_org;

    IF v_org_entry_limit IS NOT NULL THEN
      v_org_entry_count := (
        SELECT total_entry_count FROM core.organisations WHERE name = v_org
      );

      IF NEW.user_id IS NULL AND NEW.automated_service_id IS NULL
         AND v_org_entry_count + 1 > v_org_entry_limit THEN
        RAISE EXCEPTION 'Organisation entry count limit exceeded'
          USING errcode = '55P03',
                detail  = format('Organisation: %s, Org entry limit: %s, Org current count: %s',
                                 v_org, v_org_entry_limit, v_org_entry_count);
      END IF;
    END IF;
  END IF;

  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS trg_enforce_domain_quota ON storage.files;
CREATE TRIGGER trg_enforce_domain_quota
BEFORE INSERT OR UPDATE OF user_id, automated_service_id, session_id, domain, domain_id
ON storage.files
FOR EACH ROW EXECUTE FUNCTION storage.enforce_domain_quota ();

DROP TRIGGER IF EXISTS trg_enforce_domain_quota_dir ON storage.directories;
CREATE TRIGGER trg_enforce_domain_quota_dir
BEFORE INSERT OR UPDATE OF user_id, automated_service_id, session_id, domain, domain_id
ON storage.directories
FOR EACH ROW EXECUTE FUNCTION storage.enforce_domain_quota ();

-- ============================================================
-- 7. Byte-level quota gates at the file_objects stage (F11 fix)
-- ============================================================
CREATE OR REPLACE FUNCTION storage.recompute_actor_usage (p_user_id int, p_service_id int)
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  IF p_user_id IS NOT NULL THEN
    UPDATE core.users
    SET total_virtual_size = (
          SELECT coalesce(sum(o.original_size), 0)
          FROM storage.files f
          JOIN storage.file_objects fo ON fo.file_id = f.id
          JOIN storage.objects o ON o.id = fo.object_id
          WHERE f.user_id = p_user_id
        ),
        total_real_size = (
          SELECT coalesce(sum(o.size), 0)
          FROM storage.files f
          JOIN storage.file_objects fo ON fo.file_id = f.id
          JOIN storage.objects o ON o.id = fo.object_id
          WHERE f.user_id = p_user_id
        ),
        total_entry_count = (
          SELECT count(*)::bigint FROM storage.files f WHERE f.user_id = p_user_id
        ) + (
          SELECT count(*)::bigint FROM storage.directories d WHERE d.user_id = p_user_id
        )
    WHERE id = p_user_id;
  ELSIF p_service_id IS NOT NULL THEN
    UPDATE core.automated_services
    SET total_virtual_size = (
          SELECT coalesce(sum(o.original_size), 0)
          FROM storage.files f
          JOIN storage.file_objects fo ON fo.file_id = f.id
          JOIN storage.objects o ON o.id = fo.object_id
          WHERE f.automated_service_id = p_service_id
        ),
        total_real_size = (
          SELECT coalesce(sum(o.size), 0)
          FROM storage.files f
          JOIN storage.file_objects fo ON fo.file_id = f.id
          JOIN storage.objects o ON o.id = fo.object_id
          WHERE f.automated_service_id = p_service_id
        ),
        total_entry_count = (
          SELECT count(*)::bigint FROM storage.files f WHERE f.automated_service_id = p_service_id
        ) + (
          SELECT count(*)::bigint FROM storage.directories d WHERE d.automated_service_id = p_service_id
        )
    WHERE id = p_service_id;
  END IF;
END;
$$;

CREATE OR REPLACE FUNCTION storage.apply_object_owner_usage () RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE
  v_file_id     bigint;
  v_user_id     int;
  v_service_id  int;
  v_domain_id   int;
  v_org         text;
  v_usage       bigint;
  v_limit       bigint;
  v_entry_count bigint;
  v_entry_limit bigint;
  v_org_usage   bigint;
  v_org_limit   bigint;
BEGIN
  IF TG_OP = 'INSERT' OR TG_OP = 'UPDATE' THEN
    v_file_id := NEW.file_id;
  ELSE
    v_file_id := OLD.file_id;
  END IF;

  SELECT f.user_id, f.automated_service_id, f.domain_id
    INTO v_user_id, v_service_id, v_domain_id
  FROM storage.files f
  WHERE f.id = v_file_id;

  IF NOT FOUND THEN
    IF TG_OP = 'DELETE' THEN
      RETURN OLD; -- file already gone: nothing to maintain
    END IF;
    RAISE EXCEPTION 'file_objects row references missing file %', v_file_id USING errcode = '23503';
  END IF;

  IF v_user_id IS NOT NULL THEN
    SELECT organisation INTO v_org FROM core.users WHERE id = v_user_id;
  ELSIF v_service_id IS NOT NULL THEN
    SELECT organisation INTO v_org FROM core.automated_services WHERE id = v_service_id;
  ELSIF v_domain_id IS NOT NULL THEN
    SELECT organisation INTO v_org FROM core.domains WHERE id = v_domain_id;
  END IF;

  PERFORM storage.recompute_actor_usage(v_user_id, v_service_id);
  IF v_org IS NOT NULL THEN
    PERFORM core.sync_org_totals_for(v_org);
  END IF;

  IF TG_OP <> 'DELETE' THEN
    IF v_user_id IS NOT NULL THEN
      SELECT total_virtual_size, storage_limit, total_entry_count, entry_count_limit
      INTO v_usage, v_limit, v_entry_count, v_entry_limit
      FROM core.users WHERE id = v_user_id;

      IF v_limit IS NOT NULL AND coalesce(v_usage, 0) > v_limit THEN
        RAISE EXCEPTION 'Actor storage quota exceeded'
          USING errcode = '55P03',
                detail  = format('Actor limit: %s, Current: %s', v_limit, coalesce(v_usage, 0));
      END IF;
    ELSIF v_service_id IS NOT NULL THEN
      SELECT total_virtual_size, storage_limit, total_entry_count, entry_count_limit
      INTO v_usage, v_limit, v_entry_count, v_entry_limit
      FROM core.automated_services WHERE id = v_service_id;

      IF v_limit IS NOT NULL AND coalesce(v_usage, 0) > v_limit THEN
        RAISE EXCEPTION 'Actor storage quota exceeded'
          USING errcode = '55P03',
                detail  = format('Actor limit: %s, Current: %s', v_limit, coalesce(v_usage, 0));
      END IF;
    END IF;

    IF v_org IS NOT NULL THEN
      SELECT total_virtual_size, storage_limit
      INTO v_org_usage, v_org_limit
      FROM core.organisations WHERE name = v_org;

      IF v_org_limit IS NOT NULL AND coalesce(v_org_usage, 0) > v_org_limit THEN
        RAISE EXCEPTION 'Organisation storage quota exceeded'
          USING errcode = '55P03',
                detail  = format('Organisation: %s, Org limit: %s, Org current: %s',
                                 v_org, v_org_limit, coalesce(v_org_usage, 0));
      END IF;
    END IF;

    IF v_domain_id IS NOT NULL THEN
      SELECT storage_limit INTO v_limit FROM core.domains WHERE id = v_domain_id;

      IF v_limit IS NOT NULL THEN
        SELECT virtual_size INTO v_usage FROM storage.domain_usage(v_domain_id);

        IF coalesce(v_usage, 0) > v_limit THEN
          RAISE EXCEPTION 'Domain storage quota exceeded'
            USING errcode = '55P03',
                  detail  = format('Domain limit: %s, Current: %s', v_limit, coalesce(v_usage, 0));
        END IF;
      END IF;
    END IF;
  END IF;

  IF TG_OP = 'DELETE' THEN
    RETURN OLD;
  END IF;
  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS trg_apply_object_owner_usage ON storage.file_objects;
CREATE TRIGGER trg_apply_object_owner_usage
AFTER INSERT OR UPDATE OF object_id OR DELETE ON storage.file_objects
FOR EACH ROW EXECUTE FUNCTION storage.apply_object_owner_usage ();

-- ============================================================
-- 8. enforce_storage_limit: entry-count checks only (F11 fix)
-- ============================================================
CREATE OR REPLACE FUNCTION storage.enforce_storage_limit () RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE
  v_entry_count    bigint;
  v_entry_limit    bigint;
  v_org            text;
  v_org_file_count bigint;
  v_org_file_limit bigint;
BEGIN
  IF NEW.user_id IS NOT NULL THEN
    SELECT total_entry_count, entry_count_limit, organisation
    INTO v_entry_count, v_entry_limit, v_org
    FROM core.users WHERE id = NEW.user_id;
  ELSIF NEW.automated_service_id IS NOT NULL THEN
    SELECT total_entry_count, entry_count_limit, organisation
    INTO v_entry_count, v_entry_limit, v_org
    FROM core.automated_services WHERE id = NEW.automated_service_id;
  END IF;

  IF v_org IS NULL THEN
    RETURN NEW;
  END IF;

  IF v_entry_limit IS NOT NULL AND (coalesce(v_entry_count, 0) + 1) > v_entry_limit THEN
    RAISE EXCEPTION 'Entry count limit exceeded'
      USING errcode = '55P03',
            detail  = format('Actor entry limit: %s, Current count: %s',
                             v_entry_limit, coalesce(v_entry_count, 0));
  END IF;

  SELECT total_entry_count, entry_count_limit
  INTO v_org_file_count, v_org_file_limit
  FROM core.organisations WHERE name = v_org;

  IF v_org_file_limit IS NOT NULL AND (coalesce(v_org_file_count, 0) + 1) > v_org_file_limit THEN
    RAISE EXCEPTION 'Organisation entry count limit exceeded'
      USING errcode = '55P03',
            detail  = format('Organisation: %s, Org entry limit: %s, Org current count: %s',
                             v_org, v_org_file_limit, coalesce(v_org_file_count, 0));
  END IF;

  RETURN NEW;
END;
$$;

-- ============================================================
-- 9. owner usage INSERT branch disabled (was +0 bytes per upload, F11)
--    (identical to the postgres/schemas/storage.sql definition apart from
--    the INSERT guard; re-run of the statement is idempotent)
-- ============================================================
CREATE OR REPLACE FUNCTION storage.update_owner_storage_usage () RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE
  v_old_virtual bigint := 0;
  v_old_real    bigint := 0;
  v_new_virtual bigint := 0;
  v_new_real    bigint := 0;
BEGIN
  IF (TG_OP = 'INSERT') THEN
    RETURN NEW;
  ELSIF (TG_OP = 'DELETE') THEN
    SELECT virtual_size, real_size INTO v_old_virtual, v_old_real FROM storage.get_file_sizes(OLD.id);

    IF OLD.user_id IS NOT NULL THEN
      UPDATE core.users
      SET total_virtual_size = coalesce(total_virtual_size, 0) - v_old_virtual,
          total_real_size    = coalesce(total_real_size, 0)    - v_old_real,
          total_entry_count   = coalesce(total_entry_count, 0) - 1
      WHERE id = OLD.user_id;
    ELSIF OLD.automated_service_id IS NOT NULL THEN
      UPDATE core.automated_services
      SET total_virtual_size = coalesce(total_virtual_size, 0) - v_old_virtual,
          total_real_size    = coalesce(total_real_size, 0)    - v_old_real,
          total_entry_count   = coalesce(total_entry_count, 0) - 1
      WHERE id = OLD.automated_service_id;
    END IF;
    RETURN OLD;
  ELSIF (TG_OP = 'UPDATE') THEN
    IF OLD.user_id IS NOT DISTINCT FROM NEW.user_id
       AND OLD.automated_service_id IS NOT DISTINCT FROM NEW.automated_service_id THEN
      RETURN NEW;
    END IF;

    SELECT virtual_size, real_size INTO v_old_virtual, v_old_real FROM storage.get_file_sizes(OLD.id);
    SELECT virtual_size, real_size INTO v_new_virtual, v_new_real FROM storage.get_file_sizes(NEW.id);

    IF OLD.user_id IS NOT NULL THEN
      UPDATE core.users
      SET total_virtual_size = coalesce(total_virtual_size, 0) - v_old_virtual,
          total_real_size    = coalesce(total_real_size, 0)    - v_old_real,
          total_entry_count   = coalesce(total_entry_count, 0) - 1
      WHERE id = OLD.user_id;
    ELSIF OLD.automated_service_id IS NOT NULL THEN
      UPDATE core.automated_services
      SET total_virtual_size = coalesce(total_virtual_size, 0) - v_old_virtual,
          total_real_size    = coalesce(total_real_size, 0)    - v_old_real,
          total_entry_count   = coalesce(total_entry_count, 0) - 1
      WHERE id = OLD.automated_service_id;
    END IF;

    IF NEW.user_id IS NOT NULL THEN
      UPDATE core.users
      SET total_virtual_size = coalesce(total_virtual_size, 0) + v_new_virtual,
          total_real_size    = coalesce(total_real_size, 0)    + v_new_real,
          total_entry_count   = coalesce(total_entry_count, 0) + 1
      WHERE id = NEW.user_id;
    ELSIF NEW.automated_service_id IS NOT NULL THEN
      UPDATE core.automated_services
      SET total_virtual_size = coalesce(total_virtual_size, 0) + v_new_virtual,
          total_real_size    = coalesce(total_real_size, 0)    + v_new_real,
          total_entry_count   = coalesce(total_entry_count, 0) + 1
      WHERE id = NEW.automated_service_id;
    END IF;
    RETURN NEW;
  END IF;

  RETURN NULL;
END;
$$;

-- ============================================================
-- 10. rename_domain + create_organisation
-- ============================================================
CREATE OR REPLACE FUNCTION core.rename_domain (
  p_domain_id int,
  p_new_name text
) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
  v_old_name text;
  v_org text;
  v_old_path text;
  v_new_path text;
BEGIN
  IF p_new_name IS NULL OR p_new_name = '' THEN
    RAISE EXCEPTION 'Domain name must not be empty' USING errcode = '23514';
  END IF;

  IF p_new_name <> lower(p_new_name) THEN
    RAISE EXCEPTION 'Domain names are lower-case' USING errcode = '23514';
  END IF;

  IF p_new_name ~ '[.@/]' THEN
    RAISE EXCEPTION 'Domain names must not contain . @ or /' USING errcode = '23514';
  END IF;

  SELECT name, organisation, path INTO v_old_name, v_org, v_old_path
  FROM core.domains WHERE id = p_domain_id FOR UPDATE;

  IF NOT FOUND THEN
    RAISE EXCEPTION 'Domain % does not exist', p_domain_id USING errcode = '23503';
  END IF;

  IF v_old_name = p_new_name THEN
    RETURN;
  END IF;

  IF EXISTS (
    SELECT 1 FROM core.domains d
    WHERE d.organisation = v_org AND d.name = p_new_name AND d.id <> p_domain_id
  ) THEN
    RAISE EXCEPTION 'Domain label % already exists in organisation %', p_new_name, v_org
      USING errcode = '23505';
  END IF;

  -- trg_domain_path rewrites this row's own path from its (unchanged) parent.
  v_new_path := left(v_old_path, length(v_old_path) - length(v_old_name)) || p_new_name;

  -- Composite FKs on (organisation, domain) cascade the rename into
  -- users/automated_services/user_domain_permissions.
  UPDATE core.domains SET name = p_new_name WHERE id = p_domain_id;

  -- Descendants keep their own labels, but their paths embed the old prefix.
  UPDATE core.domains
     SET path = v_new_path || substring(path FROM length(v_old_path) + 1)
   WHERE organisation = v_org
     AND left(path, length(v_old_path) + 1) = v_old_path || '/';

  UPDATE storage.directories SET domain = p_new_name WHERE domain_id = p_domain_id;
  UPDATE storage.files SET domain = p_new_name WHERE domain_id = p_domain_id;
  UPDATE storage.upload_sessions SET domain = p_new_name WHERE domain_id = p_domain_id;
END;
$$;

-- Return type changes from int to TABLE(...): CREATE OR REPLACE cannot do that,
-- so drop the previous signature first (idempotent on repeat runs).
DROP FUNCTION IF EXISTS core.create_organisation (text, text, text);

CREATE OR REPLACE FUNCTION core.create_organisation (
  p_name text,
  p_description text DEFAULT NULL,
  p_domain text DEFAULT NULL,
  p_storage_limit bigint DEFAULT NULL,
  p_entry_count_limit bigint DEFAULT NULL,
  p_admin_email text DEFAULT NULL,
  p_admin_first_name text DEFAULT NULL,
  p_admin_family_name text DEFAULT NULL,
  p_admin_password text DEFAULT NULL,
  p_granted_by int DEFAULT NULL
) RETURNS TABLE (out_organisation text, out_admin_user_id int)
LANGUAGE plpgsql AS $$
DECLARE
  v_admin_id int;
BEGIN
  IF p_name IS NULL THEN
    RAISE EXCEPTION 'Organisation name must not be null' USING errcode = '23514';
  END IF;

  p_name := trim(p_name);
  IF p_name = '' THEN
    RAISE EXCEPTION 'Organisation name must not be empty' USING errcode = '23514';
  END IF;

  IF p_name ~ '[.@/]' THEN
    RAISE EXCEPTION 'Organisation names must not contain . @ or /' USING errcode = '23514';
  END IF;

  IF p_storage_limit IS NOT NULL AND p_storage_limit < 0 THEN
    RAISE EXCEPTION 'storage_limit must be null or non-negative' USING errcode = '23514';
  END IF;

  IF p_entry_count_limit IS NOT NULL AND p_entry_count_limit < 0 THEN
    RAISE EXCEPTION 'entry_count_limit must be null or non-negative' USING errcode = '23514';
  END IF;

  -- Atomic: the organisation and its optional first administrator land in the
  -- same transaction, so a duplicate name/email rolls the whole thing back and
  -- never leaves an orphan user behind.
  INSERT INTO core.organisations (name, description, domain, storage_limit, entry_count_limit)
  VALUES (p_name, p_description, nullif(trim(p_domain), ''), p_storage_limit, p_entry_count_limit);

  IF p_admin_email IS NOT NULL THEN
    IF coalesce(trim(p_admin_first_name), '') = '' OR coalesce(trim(p_admin_family_name), '') = '' THEN
      RAISE EXCEPTION 'Initial admin first and family name are required' USING errcode = '23514';
    END IF;

    IF p_admin_password IS NULL OR length(p_admin_password) < 12 THEN
      RAISE EXCEPTION 'Initial admin password must be at least 12 characters' USING errcode = '23514';
    END IF;

    -- trg_hash_password hashes the plaintext on insert.
    INSERT INTO core.users (organisation, first_name, family_name, email, password, role)
    VALUES (p_name, trim(p_admin_first_name), trim(p_admin_family_name), lower(trim(p_admin_email)), p_admin_password, 'admin')
    RETURNING id INTO v_admin_id;

    INSERT INTO core.admin_assignments (user_id, scope, organisation, granted_by)
    VALUES (v_admin_id, 'organisation', p_name, p_granted_by);
  END IF;

  RETURN QUERY SELECT p_name, v_admin_id;
END;
$$;

-- ============================================================
-- 11. Backfills
-- ============================================================
-- (a) Domain ids for rows predating the derivation trigger.  Rows whose
--     label cannot be resolved to the owner's organisation stay NULL and are
--     reported below; those namespaces must be re-homed manually.
UPDATE storage.files f
   SET domain_id = d.id
  FROM core.domains d
 WHERE f.domain_id IS NULL
   AND f.domain IS NOT NULL
   AND d.name = f.domain
   AND d.organisation = coalesce(
         (SELECT u.organisation FROM core.users u WHERE u.id = f.user_id),
         (SELECT a.organisation FROM core.automated_services a WHERE a.id = f.automated_service_id),
         (SELECT coalesce(u2.organisation, a2.organisation)
          FROM core.sessions s
          LEFT JOIN core.users u2 ON u2.id = s.user_id
          LEFT JOIN core.automated_services a2 ON a2.id = s.automated_service_id
          WHERE s.id = f.session_id));

UPDATE storage.directories f
   SET domain_id = d.id
  FROM core.domains d
 WHERE f.domain_id IS NULL
   AND f.domain IS NOT NULL
   AND d.name = f.domain
   AND d.organisation = coalesce(
         (SELECT u.organisation FROM core.users u WHERE u.id = f.user_id),
         (SELECT a.organisation FROM core.automated_services a WHERE a.id = f.automated_service_id),
         (SELECT coalesce(u2.organisation, a2.organisation)
          FROM core.sessions s
          LEFT JOIN core.users u2 ON u2.id = s.user_id
          LEFT JOIN core.automated_services a2 ON a2.id = s.automated_service_id
          WHERE s.id = f.session_id));

UPDATE storage.upload_sessions f
   SET domain_id = d.id
  FROM core.domains d
 WHERE f.domain_id IS NULL
   AND f.domain IS NOT NULL
   AND d.name = f.domain
   AND d.organisation = coalesce(
         (SELECT u.organisation FROM core.users u WHERE u.id = f.user_id),
         (SELECT a.organisation FROM core.automated_services a WHERE a.id = f.automated_service_id),
         (SELECT coalesce(u2.organisation, a2.organisation)
          FROM core.sessions s
          LEFT JOIN core.users u2 ON u2.id = s.user_id
          LEFT JOIN core.automated_services a2 ON a2.id = s.automated_service_id
          WHERE s.id = f.owner_session_id));

DO $$
DECLARE
  v_orphan bigint;
BEGIN
  SELECT count(*) INTO v_orphan FROM storage.files WHERE domain_id IS NULL AND domain IS NOT NULL;
  IF v_orphan > 0 THEN
    RAISE NOTICE 'migration 008: % storage.files row(s) keep an unresolvable domain label (domain_id left NULL)', v_orphan;
  END IF;
  SELECT count(*) INTO v_orphan FROM storage.directories WHERE domain_id IS NULL AND domain IS NOT NULL;
  IF v_orphan > 0 THEN
    RAISE NOTICE 'migration 008: % storage.directories row(s) keep an unresolvable domain label (domain_id left NULL)', v_orphan;
  END IF;
  SELECT count(*) INTO v_orphan FROM storage.upload_sessions WHERE domain_id IS NULL AND domain IS NOT NULL;
  IF v_orphan > 0 THEN
    RAISE NOTICE 'migration 008: % storage.upload_sessions row(s) keep an unresolvable domain label (domain_id left NULL)', v_orphan;
  END IF;
END
$$;

-- (b) Recompute owner/org aggregates once so the corrected accounting starts
--     from truthful numbers (the legacy path wrote +0 bytes per upload).
DO $$
DECLARE
  r record;
BEGIN
  FOR r IN SELECT id FROM core.users WHERE deleted_at IS NULL LOOP
    PERFORM storage.recompute_actor_usage(r.id, NULL);
  END LOOP;
  FOR r IN SELECT id FROM core.automated_services WHERE deleted_at IS NULL LOOP
    PERFORM storage.recompute_actor_usage(NULL, r.id);
  END LOOP;
  FOR r IN SELECT name FROM core.organisations LOOP
    PERFORM core.sync_org_totals_for(r.name);
  END LOOP;
END
$$;

COMMIT;

-- Summary probe: apply/verify helper for operators.
SELECT 'migration 008 applied' AS status;