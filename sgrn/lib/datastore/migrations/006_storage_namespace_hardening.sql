-- Storage namespace hardening.  This migration is intentionally additive: the
-- application continues to populate `domain` while domain_id is backfilled and
-- adopted by the resolver in the follow-up deployment.
BEGIN;

ALTER TABLE storage.files ADD COLUMN IF NOT EXISTS domain_id integer REFERENCES core.domains(id) ON DELETE RESTRICT;
ALTER TABLE storage.directories ADD COLUMN IF NOT EXISTS domain_id integer REFERENCES core.domains(id) ON DELETE RESTRICT;
ALTER TABLE storage.upload_sessions ADD COLUMN IF NOT EXISTS domain_id integer REFERENCES core.domains(id) ON DELETE RESTRICT;
ALTER TABLE storage.files ADD COLUMN IF NOT EXISTS content_sha256 text;

-- Resolve legacy bare names using the owner's organisation where possible.
UPDATE storage.files f
SET domain_id = d.id
FROM core.users u, core.domains d
WHERE f.domain IS NOT NULL AND f.domain_id IS NULL AND f.user_id = u.id
  AND d.organisation = u.organisation AND d.name = f.domain;

UPDATE storage.files f
SET domain_id = d.id
FROM core.automated_services s, core.domains d
WHERE f.domain IS NOT NULL AND f.domain_id IS NULL AND f.automated_service_id = s.id
  AND d.organisation = s.organisation AND d.name = f.domain;

UPDATE storage.directories drow
SET domain_id = d.id
FROM core.users u, core.domains d
WHERE drow.domain IS NOT NULL AND drow.domain_id IS NULL AND drow.user_id = u.id
  AND d.organisation = u.organisation AND d.name = drow.domain;

-- Personal and domain namespaces are distinct.  The previous indexes omitted
-- the domain predicate and made /x collide with a domain's /x.
DROP INDEX IF EXISTS storage.uq_files_root;
DROP INDEX IF EXISTS storage.uq_files_root_service;
DROP INDEX IF EXISTS storage.uq_files_directory;
DROP INDEX IF EXISTS storage.uq_files_directory_service;
CREATE UNIQUE INDEX uq_files_root ON storage.files (user_id, name)
  WHERE directory_id IS NULL AND user_id IS NOT NULL AND domain IS NULL;
CREATE UNIQUE INDEX uq_files_root_service ON storage.files (automated_service_id, name)
  WHERE directory_id IS NULL AND automated_service_id IS NOT NULL AND domain IS NULL;
CREATE UNIQUE INDEX uq_files_directory ON storage.files (user_id, directory_id, name)
  WHERE directory_id IS NOT NULL AND user_id IS NOT NULL AND domain IS NULL;
CREATE UNIQUE INDEX uq_files_directory_service ON storage.files (automated_service_id, directory_id, name)
  WHERE directory_id IS NOT NULL AND automated_service_id IS NOT NULL AND domain IS NULL;

CREATE OR REPLACE FUNCTION core.domain_canonical(p_domain_id integer) RETURNS text
LANGUAGE sql STABLE AS $$
    SELECT '@' || d.organisation || '.' || replace(ltrim(d.path, '/'), '/', '.')
    FROM core.domains d WHERE d.id = p_domain_id
$$;

COMMIT;
