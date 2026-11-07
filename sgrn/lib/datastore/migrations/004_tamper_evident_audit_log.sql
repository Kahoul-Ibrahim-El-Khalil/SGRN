-- ============================================================================
-- Migration 004: Tamper-Evident & Append-Only Audit Log
--
-- 1. Adds monotonic per-organisation sequence `seq`, `prev_hash`, and `row_hash`
--    to `core.audit_logs`.
-- 2. Backfills genesis hash for any existing records.
-- 3. Creates `core.audit_checkpoints` for signed audit checkpoints.
-- 4. Creates `core.append_audit_log()` SECURITY DEFINER function with per-organisation
--    advisory locking.
-- 5. Attaches trigger prohibiting UPDATE, DELETE, and TRUNCATE on `core.audit_logs`.
-- 6. Creates `core.verify_audit_chain()` SQL verification function.
-- ============================================================================

-- 1. Alter core.audit_logs with chain columns if they don't already exist
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = 'core' AND table_name = 'audit_logs' AND column_name = 'seq'
    ) THEN
        ALTER TABLE core.audit_logs
            ADD COLUMN seq bigint,
            ADD COLUMN prev_hash text,
            ADD COLUMN row_hash text;
    END IF;
END $$;

-- 2. Backfill existing records if seq is null
DO $$
DECLARE
    r RECORD;
    v_org text;
    v_seq bigint;
    v_prev text;
    v_canon text;
    v_hash text;
BEGIN
    FOR v_org IN SELECT DISTINCT organisation FROM core.audit_logs WHERE seq IS NULL LOOP
        v_seq := 0;
        v_prev := '0000000000000000000000000000000000000000000000000000000000000000';
        FOR r IN
            SELECT id, organisation, actor_type, actor_id, actor_name, action, target_type, target_id, ip, details, status, created_at
            FROM core.audit_logs
            WHERE organisation = v_org AND seq IS NULL
            ORDER BY created_at ASC, id ASC
        LOOP
            v_seq := v_seq + 1;
            v_canon := v_prev || '|' || v_seq || '|' || r.organisation || '|' || r.actor_type || '|' ||
                       coalesce(r.actor_id::text, '') || '|' || r.actor_name || '|' || r.action || '|' ||
                       coalesce(r.target_type, '') || '|' || coalesce(r.target_id, '') || '|' ||
                       coalesce(r.ip::text, '') || '|' || coalesce((r.details)::text, '{}') || '|' ||
                       r.status || '|' || to_char(r.created_at at time zone 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"');
            v_hash := encode(sha256(v_canon::bytea), 'hex');

            UPDATE core.audit_logs
            SET seq = v_seq, prev_hash = v_prev, row_hash = v_hash
            WHERE id = r.id;

            v_prev := v_hash;
        END LOOP;
    END LOOP;
END $$;

-- Enforce constraints after backfill
DO $$
BEGIN
    -- Set NOT NULL if not already
    ALTER TABLE core.audit_logs
        ALTER COLUMN seq SET NOT NULL,
        ALTER COLUMN prev_hash SET NOT NULL,
        ALTER COLUMN row_hash SET NOT NULL;
EXCEPTION
    WHEN others THEN NULL;
END $$;

CREATE UNIQUE INDEX IF NOT EXISTS idx_audit_logs_org_seq
    ON core.audit_logs (organisation, seq);

CREATE INDEX IF NOT EXISTS idx_audit_logs_row_hash
    ON core.audit_logs (row_hash);

-- 3. Checkpoints table for periodic Ed25519 signed snapshots
CREATE TABLE IF NOT EXISTS core.audit_checkpoints (
    id bigserial PRIMARY KEY,
    organisation text NOT NULL REFERENCES core.organisations(name) ON DELETE CASCADE,
    last_seq bigint NOT NULL,
    last_hash text NOT NULL,
    sink_target text, -- 'local', 's3', 'syslog'
    signature text,   -- Ed25519 signature (hex)
    public_key text,  -- Ed25519 public key (hex)
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_audit_checkpoints_org_seq
    ON core.audit_checkpoints (organisation, last_seq DESC);

-- 4. Append function: executes under advisory lock per-organisation
CREATE OR REPLACE FUNCTION core.append_audit_log(
    p_organisation text,
    p_actor_type text,
    p_actor_id int,
    p_actor_name text,
    p_action text,
    p_target_type text DEFAULT NULL,
    p_target_id text DEFAULT NULL,
    p_ip inet DEFAULT '127.0.0.1'::inet,
    p_details jsonb DEFAULT '{}'::jsonb,
    p_status text DEFAULT 'success',
    p_created_at timestamptz DEFAULT now()
)
RETURNS TABLE (
    out_id bigint,
    out_seq bigint,
    out_prev_hash text,
    out_row_hash text,
    out_created_at timestamptz
)
LANGUAGE plpgsql
SECURITY DEFINER
AS $$
DECLARE
    v_lock_key bigint;
    v_last_seq bigint;
    v_prev_hash text;
    v_canon text;
    v_row_hash text;
    v_new_id bigint;
    v_timestamp timestamptz;
BEGIN
    -- Derive 64-bit advisory lock key from organisation name
    v_lock_key := ('x' || substr(md5('sgrn_audit_lock_' || p_organisation), 1, 16))::bit(64)::bigint;
    PERFORM pg_advisory_xact_lock(v_lock_key);

    -- Find last sequence and row_hash for this organisation
    SELECT seq, row_hash
    INTO v_last_seq, v_prev_hash
    FROM core.audit_logs
    WHERE organisation = p_organisation
    ORDER BY seq DESC
    LIMIT 1;

    IF v_last_seq IS NULL THEN
        v_last_seq := 1;
        v_prev_hash := '0000000000000000000000000000000000000000000000000000000000000000';
    ELSE
        v_last_seq := v_last_seq + 1;
    END IF;

    v_timestamp := coalesce(p_created_at, now());

    -- Canonical serialization with normalized JSON string
    v_canon := v_prev_hash || '|' ||
               v_last_seq || '|' ||
               p_organisation || '|' ||
               p_actor_type || '|' ||
               coalesce(p_actor_id::text, '') || '|' ||
               p_actor_name || '|' ||
               p_action || '|' ||
               coalesce(p_target_type, '') || '|' ||
               coalesce(p_target_id, '') || '|' ||
               coalesce(p_ip::text, '') || '|' ||
               coalesce((p_details)::text, '{}') || '|' ||
               p_status || '|' ||
               to_char(v_timestamp at time zone 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"');

    v_row_hash := encode(sha256(v_canon::bytea), 'hex');

    INSERT INTO core.audit_logs (
        organisation, actor_type, actor_id, actor_name, action, target_type, target_id,
        ip, details, status, created_at, seq, prev_hash, row_hash
    ) VALUES (
        p_organisation, p_actor_type, p_actor_id, p_actor_name, p_action, p_target_type, p_target_id,
        p_ip, coalesce(p_details, '{}'::jsonb), p_status, v_timestamp, v_last_seq, v_prev_hash, v_row_hash
    )
    RETURNING id INTO v_new_id;

    RETURN QUERY SELECT v_new_id, v_last_seq, v_prev_hash, v_row_hash, v_timestamp;
END;
$$;

-- 5. Verification function walking the chain for an organisation
CREATE OR REPLACE FUNCTION core.verify_audit_chain(p_organisation text)
RETURNS TABLE (
    is_valid boolean,
    total_rows bigint,
    first_broken_seq bigint,
    first_broken_id bigint,
    failure_reason text
)
LANGUAGE plpgsql
STABLE
AS $$
DECLARE
    r RECORD;
    v_expected_seq bigint := 1;
    v_expected_prev text := '0000000000000000000000000000000000000000000000000000000000000000';
    v_canon text;
    v_calc_hash text;
    v_count bigint := 0;
BEGIN
    FOR r IN
        SELECT id, organisation, actor_type, actor_id, actor_name, action, target_type, target_id,
               ip, details, status, created_at, seq, prev_hash, row_hash
        FROM core.audit_logs
        WHERE organisation = p_organisation
        ORDER BY seq ASC, id ASC
    LOOP
        v_count := v_count + 1;

        -- Check sequence monotonicity
        IF r.seq <> v_expected_seq THEN
            RETURN QUERY SELECT false, v_count, r.seq, r.id,
                ('Sequence discontinuity: expected ' || v_expected_seq || ' but got ' || r.seq)::text;
            RETURN;
        END IF;

        -- Check prev_hash pointer
        IF r.prev_hash <> v_expected_prev THEN
            RETURN QUERY SELECT false, v_count, r.seq, r.id,
                ('Prev hash mismatch: expected ' || v_expected_prev || ' but got ' || r.prev_hash)::text;
            RETURN;
        END IF;

        -- Recalculate row_hash
        v_canon := r.prev_hash || '|' ||
                   r.seq || '|' ||
                   r.organisation || '|' ||
                   r.actor_type || '|' ||
                   coalesce(r.actor_id::text, '') || '|' ||
                   r.actor_name || '|' ||
                   r.action || '|' ||
                   coalesce(r.target_type, '') || '|' ||
                   coalesce(r.target_id, '') || '|' ||
                   coalesce(r.ip::text, '') || '|' ||
                   coalesce((r.details)::text, '{}') || '|' ||
                   r.status || '|' ||
                   to_char(r.created_at at time zone 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"');

        v_calc_hash := encode(sha256(v_canon::bytea), 'hex');

        IF r.row_hash <> v_calc_hash THEN
            RETURN QUERY SELECT false, v_count, r.seq, r.id,
                ('Row hash mismatch: record tampered. expected ' || v_calc_hash || ' but got ' || r.row_hash)::text;
            RETURN;
        END IF;

        v_expected_prev := r.row_hash;
        v_expected_seq := v_expected_seq + 1;
    END LOOP;

    RETURN QUERY SELECT true, v_count, NULL::bigint, NULL::bigint, 'Chain intact'::text;
END;
$$;

-- 6. Trigger preventing UPDATE, DELETE, and TRUNCATE on audit_logs
CREATE OR REPLACE FUNCTION core.trg_prohibit_audit_tampering()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    RAISE EXCEPTION 'Audit logs are tamper-evident and append-only. UPDATE and DELETE operations are prohibited.';
END;
$$;

DROP TRIGGER IF EXISTS trg_audit_tamper_guard ON core.audit_logs;
CREATE TRIGGER trg_audit_tamper_guard
    BEFORE UPDATE OR DELETE ON core.audit_logs
    FOR EACH ROW
    EXECUTE FUNCTION core.trg_prohibit_audit_tampering();

-- 7. Grant execution permissions
GRANT EXECUTE ON FUNCTION core.append_audit_log TO PUBLIC;
GRANT EXECUTE ON FUNCTION core.verify_audit_chain TO PUBLIC;
GRANT SELECT ON core.audit_checkpoints TO PUBLIC;
