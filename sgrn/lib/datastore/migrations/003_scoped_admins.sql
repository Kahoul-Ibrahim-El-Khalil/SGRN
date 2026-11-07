-- SGRN migration 003: Three admin levels (platform / organisation / domain)
-- and hierarchical domain trees.
--
-- CONVENTIONS: Idempotent (safe to run multiple times).
-- Applies cleanly on top of 001 and 002.

-- ============================================================
-- 1. core.users.is_platform_admin
-- ============================================================
ALTER TABLE core.users
  ADD COLUMN IF NOT EXISTS is_platform_admin boolean NOT NULL DEFAULT false;

COMMENT ON COLUMN core.users.role IS
  'DEPRECATED: Retained for backward compatibility. Authoritative access control is resolved via core.admin_assignments and core.admin_level().';

-- ============================================================
-- 2. core.domains: hierarchy (parent_id, path) & triggers
-- ============================================================
ALTER TABLE core.domains
  ADD COLUMN IF NOT EXISTS parent_id int REFERENCES core.domains(id) ON DELETE RESTRICT DEFAULT NULL;

ALTER TABLE core.domains
  ADD COLUMN IF NOT EXISTS path text NOT NULL DEFAULT '';

-- Backfill existing root domains as '/' || name if path is empty
UPDATE core.domains
SET path = '/' || name
WHERE path = '' OR path IS NULL;

-- Unique constraint on (organisation, path)
CREATE UNIQUE INDEX IF NOT EXISTS uq_domains_org_path
  ON core.domains (organisation, path);

-- Trigger function maintaining path as parent.path || '/' || name
CREATE OR REPLACE FUNCTION core.trg_maintain_domain_path()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
  v_parent_org text;
  v_parent_path text;
BEGIN
  IF NEW.parent_id IS NULL THEN
    NEW.path := '/' || NEW.name;
  ELSE
    SELECT organisation, path INTO v_parent_org, v_parent_path
    FROM core.domains WHERE id = NEW.parent_id;

    IF NOT FOUND THEN
      RAISE EXCEPTION 'Parent domain id % does not exist', NEW.parent_id;
    END IF;

    IF v_parent_org != NEW.organisation THEN
      RAISE EXCEPTION 'Parent domain % belongs to organisation %, expected %',
        NEW.parent_id, v_parent_org, NEW.organisation;
    END IF;

    NEW.path := v_parent_path || '/' || NEW.name;
  END IF;

  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS trg_domain_path ON core.domains;
CREATE TRIGGER trg_domain_path
BEFORE INSERT OR UPDATE OF name, parent_id, organisation ON core.domains
FOR EACH ROW EXECUTE FUNCTION core.trg_maintain_domain_path();

-- Subtree relocation function: updates parent and rewrites descendant paths atomically
CREATE OR REPLACE FUNCTION core.move_domain_subtree(
  p_domain_id int,
  p_new_parent_id int
) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
  v_org text;
  v_name text;
  v_old_path text;
  v_new_path text;
  v_parent_org text;
  v_parent_path text;
BEGIN
  SELECT organisation, name, path INTO v_org, v_name, v_old_path
  FROM core.domains WHERE id = p_domain_id;

  IF NOT FOUND THEN
    RAISE EXCEPTION 'Domain % does not exist', p_domain_id;
  END IF;

  IF p_new_parent_id IS NOT NULL THEN
    IF p_new_parent_id = p_domain_id THEN
      RAISE EXCEPTION 'Domain cannot be parent of itself';
    END IF;

    SELECT organisation, path INTO v_parent_org, v_parent_path
    FROM core.domains WHERE id = p_new_parent_id;

    IF NOT FOUND THEN
      RAISE EXCEPTION 'Target parent domain % does not exist', p_new_parent_id;
    END IF;

    IF v_parent_org != v_org THEN
      RAISE EXCEPTION 'Target parent domain belongs to different organisation';
    END IF;

    -- Cycle check: target parent cannot be inside subtree of p_domain_id
    IF v_parent_path = v_old_path OR left(v_parent_path, length(v_old_path) + 1) = v_old_path || '/' THEN
      RAISE EXCEPTION 'Cannot move domain into its own descendant subtree';
    END IF;

    v_new_path := v_parent_path || '/' || v_name;
  ELSE
    v_new_path := '/' || v_name;
  END IF;

  -- 1. Move root of the subtree
  UPDATE core.domains
  SET parent_id = p_new_parent_id,
      path = v_new_path
  WHERE id = p_domain_id;

  -- 2. Atomically rewrite all descendant paths
  UPDATE core.domains
  SET path = v_new_path || substring(path from length(v_old_path) + 1)
  WHERE organisation = v_org
    AND left(path, length(v_old_path) + 1) = v_old_path || '/';
END;
$$;

-- ============================================================
-- 3. core.admin_assignments table
-- ============================================================
CREATE TABLE IF NOT EXISTS core.admin_assignments (
  id bigserial PRIMARY KEY,
  user_id int NOT NULL REFERENCES core.users(id) ON DELETE CASCADE,
  scope text NOT NULL CHECK (scope IN ('organisation', 'domain')),
  organisation text NOT NULL REFERENCES core.organisations(name) ON DELETE CASCADE ON UPDATE CASCADE,
  domain_id int REFERENCES core.domains(id) ON DELETE CASCADE,
  can_manage_users boolean NOT NULL DEFAULT true,
  can_manage_keys boolean NOT NULL DEFAULT true,
  granted_by int REFERENCES core.users(id) ON DELETE SET NULL,
  created_at timestamptz NOT NULL DEFAULT now(),
  CONSTRAINT chk_admin_assignment_scope CHECK (
    (scope = 'organisation' AND domain_id IS NULL) OR
    (scope = 'domain' AND domain_id IS NOT NULL)
  )
);

CREATE UNIQUE INDEX IF NOT EXISTS uq_admin_assignment
  ON core.admin_assignments (user_id, scope, organisation, coalesce(domain_id, 0));

CREATE INDEX IF NOT EXISTS idx_admin_assignments_user
  ON core.admin_assignments (user_id);

CREATE INDEX IF NOT EXISTS idx_admin_assignments_org
  ON core.admin_assignments (organisation);

CREATE INDEX IF NOT EXISTS idx_admin_assignments_domain
  ON core.admin_assignments (domain_id) WHERE domain_id IS NOT NULL;

-- ============================================================
-- 4. core.admin_level(user_id, org, domain_id)
-- ============================================================
-- Returns:
--   3 = Platform Administrator
--   2 = Organisation Administrator
--   1 = Domain (or Domain Ancestor) Administrator
--   0 = None
CREATE OR REPLACE FUNCTION core.admin_level(
  p_user_id int,
  p_org text,
  p_domain_id int DEFAULT NULL
) RETURNS int LANGUAGE plpgsql STABLE AS $$
DECLARE
  v_is_platform boolean;
  v_user_deleted timestamptz;
  v_target_path text;
  v_target_org text;
BEGIN
  -- 1. Check user exists, not soft-deleted, and platform admin status
  SELECT is_platform_admin, deleted_at INTO v_is_platform, v_user_deleted
  FROM core.users WHERE id = p_user_id;

  IF NOT FOUND OR v_user_deleted IS NOT NULL THEN
    RETURN 0;
  END IF;

  IF v_is_platform THEN
    RETURN 3;
  END IF;

  IF p_org IS NULL THEN
    RETURN 0;
  END IF;

  -- 2. Check organisation admin assignment
  IF EXISTS (
    SELECT 1 FROM core.admin_assignments
    WHERE user_id = p_user_id
      AND scope = 'organisation'
      AND organisation = p_org
  ) THEN
    RETURN 2;
  END IF;

  -- 3. Check domain admin assignment (if p_domain_id provided)
  IF p_domain_id IS NOT NULL THEN
    SELECT organisation, path INTO v_target_org, v_target_path
    FROM core.domains WHERE id = p_domain_id;

    IF NOT FOUND OR v_target_org != p_org THEN
      RETURN 0;
    END IF;

    -- Compare paths exactly without LIKE:
    -- assign_path = target_path OR left(target_path, length(assign_path) + 1) = assign_path || '/'
    IF EXISTS (
      SELECT 1
      FROM core.admin_assignments a
      JOIN core.domains d ON d.id = a.domain_id
      WHERE a.user_id = p_user_id
        AND a.scope = 'domain'
        AND a.organisation = p_org
        AND (
          d.path = v_target_path
          OR (
            length(v_target_path) > length(d.path)
            AND left(v_target_path, length(d.path) + 1) = d.path || '/'
          )
        )
    ) THEN
      RETURN 1;
    END IF;
  END IF;

  RETURN 0;
END;
$$;

-- ============================================================
-- 5. Data Migration: seed admin & role='admin' backfill
-- ============================================================
-- Migrate existing users.role='admin' rows to organisation-level assignments
INSERT INTO core.admin_assignments (user_id, scope, organisation, domain_id, can_manage_users, can_manage_keys, granted_by, created_at)
SELECT u.id, 'organisation', u.organisation, NULL, true, true, NULL, now()
FROM core.users u
WHERE u.role = 'admin' AND u.deleted_at IS NULL
ON CONFLICT (user_id, scope, organisation, coalesce(domain_id, 0)) DO NOTHING;

-- Seed admin becomes platform admin (nobody else becomes platform admin automatically)
UPDATE core.users
SET is_platform_admin = true
WHERE email = 'admin@local.com';
