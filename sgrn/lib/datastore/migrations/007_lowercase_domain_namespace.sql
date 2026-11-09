-- Canonical namespace policy: domain labels are lower-case.  Existing rows
-- are normalized before the constraints are installed; a case-fold collision
-- aborts the migration and must be resolved by the operator first.
BEGIN;

DO $$
BEGIN
  IF EXISTS (
    SELECT 1
    FROM core.domains
    GROUP BY organisation, lower(name)
    HAVING count(*) > 1
  ) THEN
    RAISE EXCEPTION 'Cannot lowercase domains: case-folded organisation/name collision exists';
  END IF;
END
$$;

UPDATE core.domains SET name = lower(name), path = lower(path);
UPDATE core.users SET domain = lower(domain) WHERE domain IS NOT NULL;
UPDATE core.automated_services SET domain = lower(domain) WHERE domain IS NOT NULL;
UPDATE core.user_domain_permissions SET domain = lower(domain);

DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint
    WHERE conrelid = 'core.domains'::regclass AND conname = 'chk_domains_name_lower'
  ) THEN
    ALTER TABLE core.domains ADD CONSTRAINT chk_domains_name_lower CHECK (name = lower(name));
  END IF;
  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint
    WHERE conrelid = 'core.users'::regclass AND conname = 'chk_users_domain_lower'
  ) THEN
    ALTER TABLE core.users ADD CONSTRAINT chk_users_domain_lower CHECK (domain IS NULL OR domain = lower(domain));
  END IF;
  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint
    WHERE conrelid = 'core.automated_services'::regclass AND conname = 'chk_automated_service_domain_lower'
  ) THEN
    ALTER TABLE core.automated_services ADD CONSTRAINT chk_automated_service_domain_lower
      CHECK (domain IS NULL OR domain = lower(domain));
  END IF;
END
$$;

COMMIT;
