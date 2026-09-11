-- ============================================================
-- seed_dev_service.sql
-- Creates a fixed-credential development service for local development.
--
-- These are hardcoded so configs never need to change.
-- Safe to re-run: old rows are deleted first.
-- ============================================================

DO $$
DECLARE
    v_service_id INT;
    v_org        TEXT;
    -- Fixed public token (UUID) — used as the "public_token" in node configs
    v_token      UUID  := '1e78dbe3-1f5b-404f-89c1-ae0c07e98c5c';
    -- Fixed private token — used as "private_token" in node configs
    v_secret     TEXT  := 'FEcPKSxEuJzpst-MYcR8OlQWzFYxPoVDK9Sk3qmGn6A';
BEGIN
    -- Resolve first available organisation
    SELECT name INTO v_org FROM core.organisations ORDER BY name ASC LIMIT 1;
    IF v_org IS NULL THEN
        RAISE EXCEPTION 'No organisations exist. Run seeding.sql first.';
    END IF;

    DELETE FROM core.automated_services WHERE name = 'dev-service';

    -- The trigger trg_hash_automated_service_secret will hash the secret on INSERT.
    INSERT INTO core.automated_services (
        organisation, name, token, token_secret_hash, metadata, status
    ) VALUES (
        v_org,
        'dev-service',
        v_token,
        v_secret,   -- trigger hashes this before storage
        '{"purpose": "This is for testing", "modes": ["FULL_TREE", "BATCH_OF_FULL_TREES", "BATCH_FULL_FIRST_TREE_AND_DELTAS"]}'::jsonb,
        'active'
    )
    RETURNING id INTO v_service_id;

    RAISE NOTICE '==============================================';
    RAISE NOTICE ' Dev Service Created';
    RAISE NOTICE '==============================================';
    RAISE NOTICE '  ID            : %', v_service_id;
    RAISE NOTICE '  Organisation  : %', v_org;
    RAISE NOTICE '  public_token  : %', v_token;
    RAISE NOTICE '  private_token : %', v_secret;
    RAISE NOTICE '==============================================';



EXCEPTION WHEN OTHERS THEN
    RAISE EXCEPTION 'seed_dev_service.sql failed: % (SQLSTATE: %)', SQLERRM, SQLSTATE;
END $$;
