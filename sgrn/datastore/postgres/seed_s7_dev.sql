-- ============================================================
-- seed_s7_dev.sql
-- Creates a fixed-credential S7 development service and
-- default telemetry objects for all three snapshot modes.
--
-- Public  token (UUID) : 1311e481-cbc4-4249-a4fe-f27ff47ce4d0
-- Private token (secret): Vf_TlLKhyKV3UpGTO12dFAqmsqLvs-mxlmIgZyuG5Sg
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

    -- Clean up any previous dev service to allow re-seeding
    DELETE FROM core.automated_services WHERE name = 's7-dev-service';

    -- Insert the service with fixed token + hashed secret
    -- The trigger trg_hash_automated_service_secret will hash the secret on INSERT.
    INSERT INTO core.automated_services (
        organisation, name, token, token_secret_hash, metadata, status
    ) VALUES (
        v_org,
        's7-dev-service',
        v_token,
        v_secret,   -- trigger hashes this before storage
        '{"purpose": "gateway_dev", "modes": ["FULL_TREE", "BATCH_OF_FULL_TREES", "BATCH_FULL_FIRST_TREE_AND_DELTAS"]}'::jsonb,
        'active'
    )
    RETURNING id INTO v_service_id;

    RAISE NOTICE '==============================================';
    RAISE NOTICE ' S7 Dev Service Created';
    RAISE NOTICE '==============================================';
    RAISE NOTICE '  ID            : %', v_service_id;
    RAISE NOTICE '  Organisation  : %', v_org;
    RAISE NOTICE '  public_token  : %', v_token;
    RAISE NOTICE '  private_token : %', v_secret;
    RAISE NOTICE '==============================================';

    -- Create three default telemetry objects, one per snapshot mode.
    -- Using ON CONFLICT (automated_service_id, name) so they survive re-runs
    -- if the service ID changes (it won't, since we delete+re-insert).

    -- Object for FULL_TREE mode
    INSERT INTO telemetry."object" (
        organisation, automated_service_id, name, metadata, status
    ) VALUES (
        v_org, v_service_id, 'gateway_full',
        '{"description": "Full tree snapshot mode", "plc_model": "S7-300"}'::jsonb,
        'online'
    );
    RAISE NOTICE '  Object created: gateway_full  (FULL_TREE)';

    -- Object for BATCH_OF_FULL_TREES mode
    INSERT INTO telemetry."object" (
        organisation, automated_service_id, name, metadata, status
    ) VALUES (
        v_org, v_service_id, 'gateway_batch',
        '{"description": "Batch of full trees mode", "plc_model": "S7-300"}'::jsonb,
        'online'
    );
    RAISE NOTICE '  Object created: gateway_batch (BATCH_OF_FULL_TREES)';

    -- Object for BATCH_FULL_FIRST_TREE_AND_DELTAS mode (preferred)
    INSERT INTO telemetry."object" (
        organisation, automated_service_id, name, metadata, status
    ) VALUES (
        v_org, v_service_id, 'gateway_delta',
        '{"description": "Delta batch mode (preferred)", "plc_model": "S7-300"}'::jsonb,
        'online'
    );
    RAISE NOTICE '  Object created: gateway_delta (BATCH_FULL_FIRST_TREE_AND_DELTAS)';

    RAISE NOTICE '==============================================';
    RAISE NOTICE ' Done. You can now launch any gateway config.';
    RAISE NOTICE '==============================================';

EXCEPTION WHEN OTHERS THEN
    RAISE EXCEPTION 'seed_s7_dev.sql failed: % (SQLSTATE: %)', SQLERRM, SQLSTATE;
END $$;
