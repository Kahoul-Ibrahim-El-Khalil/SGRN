-- ============================================================================
-- Migration 005: Encrypted Storage — per-domain policy & per-file key linkage
--
-- 1. Adds `encryption_policy` ('none','optional','required') to core.domains.
-- 2. Adds `key_id` and `is_encrypted` to storage.files.
-- 3. Adds unique index on storage.files.key_id (not null rows only).
--
-- No key material is ever stored in Postgres.
-- Key bytes live exclusively in $SGRN_DATA_DIR/keys/keys.db (mode 0600).
-- ============================================================================

-- 1. encryption_policy on core.domains
ALTER TABLE core.domains
    ADD COLUMN IF NOT EXISTS encryption_policy text NOT NULL DEFAULT 'none'
        CHECK (encryption_policy IN ('none', 'optional', 'required'));

-- 2. key_id and is_encrypted on storage.files
ALTER TABLE storage.files
    ADD COLUMN IF NOT EXISTS key_id text DEFAULT NULL,
    ADD COLUMN IF NOT EXISTS is_encrypted boolean NOT NULL DEFAULT false;

-- 3. Unique partial index (allow multiple NULL key_ids, but each non-null must be unique)
CREATE UNIQUE INDEX IF NOT EXISTS uq_files_key_id
    ON storage.files (key_id)
    WHERE key_id IS NOT NULL;

-- 4. Constraint: if is_encrypted=true then key_id must be set
ALTER TABLE storage.files
    DROP CONSTRAINT IF EXISTS chk_files_encryption_consistency;
ALTER TABLE storage.files
    ADD CONSTRAINT chk_files_encryption_consistency
        CHECK (
            (is_encrypted = false AND key_id IS NULL)
            OR
            (is_encrypted = true AND key_id IS NOT NULL)
        );

-- 5. Track pending encryption keys in upload_sessions for cleanup
ALTER TABLE storage.upload_sessions
    ADD COLUMN IF NOT EXISTS is_encrypted boolean NOT NULL DEFAULT false,
    ADD COLUMN IF NOT EXISTS pending_key_id text DEFAULT NULL;
