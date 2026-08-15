-- ============================================================
-- seed_test_automated_service.sql
-- Seeds a dedicated test automated service and telemetry object for SDK testing.
-- ============================================================
DO $$
DECLARE
    v_automated_service_id INT;
    v_token UUID;
    v_secret TEXT;
    v_org TEXT;
    v_object_id INT;
BEGIN
    -- 0. Clean up existing test data to ensure a fresh run
    DELETE FROM core.automated_services WHERE name = 'sdk-test-service';
    
    -- 1. Create the test automated service
    -- We use a fixed name for the test service to make it easy to clean up or re-use.
    -- core.create_automated_service returns table
    SELECT id, token, token_secret, organisation INTO v_automated_service_id, v_token, v_secret, v_org
    FROM core.create_automated_service(
        'sdk-test-service'::varchar, 
        '{"purpose": "sdk_integration_test"}'::jsonb,
        NULL::text
    );

    RAISE NOTICE 'Test Automated Service Created:';
    RAISE NOTICE '  ID: %', v_automated_service_id;
    RAISE NOTICE '  Token: %', v_token;
    RAISE NOTICE '  Secret: %', v_secret;

    INSERT INTO telemetry."object" (organisation, automated_service_id, name, metadata, status)
    VALUES (v_org, v_automated_service_id, 'test-plc-01', '{"mac": "00:11:22:33:44:55", "model": "S7-1200"}'::jsonb, 'online')
    RETURNING id INTO v_object_id;

    RAISE NOTICE 'Test telemetry object Created:';
    RAISE NOTICE '  ID: %', v_object_id;
    RAISE NOTICE '  Name: test-plc-01';

    -- 3. Output credentials
END $$;
