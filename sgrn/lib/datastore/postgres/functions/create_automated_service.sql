-- ============================================================
-- core.gen_token(n_bytes)
-- ============================================================
-- generates a cryptographically secure base64url-encoded token
-- of n_bytes random bytes (no padding, url-safe).
-- ============================================================
create or replace function core.gen_token (n_bytes int) returns text language sql security definer
set
  search_path = core,
  public as $$
    select rtrim(
        replace(replace(replace(
            encode(gen_random_bytes(n_bytes), 'base64'),
        E'\n', ''), '+', '-'), '/', '_'),
    '=');
$$;

revoke all on function core.gen_token (int)
from
  public;

grant
execute on function core.gen_token (int) to sgrn_datastore,
sgrn_postgrest;

-- ============================================================
-- core.create_automated_service(name, metadata[, organisation, storage_limit, domain])
-- ============================================================
-- creates a new automated service and automatically generates a unique token
-- and a random token_secret.
--
-- returns the full automated service record, including the raw (unhashed)
-- token_secret. this is the only time the secret is ever
-- exposed in plain text.
--
-- token        : native UUID
-- token_secret : 32 bytes → 43 base64url chars (private secret)
-- ============================================================
create or replace function core.create_automated_service (
  p_name varchar(128),
  p_metadata jsonb default '{}',
  p_organisation text default null,
  p_storage_limit bigint default null,
  p_domain text default null
) returns table (
  id int,
  organisation text,
  name text,
  token uuid,
  token_secret text,
  status text,
  metadata jsonb,
  created_at timestamptz,
  updated_at timestamptz,
  domain text
) language plpgsql security definer
set
  search_path = core,
  public as $$
declare
    v_token  uuid;
    v_secret text;
    v_id     int;
    v_org text;
begin
    -- Resolve organisation (required).
    v_org := coalesce(
        p_organisation,
        (select orgs.name from core.organisations orgs order by orgs.name asc limit 1)
    );
    if v_org is null then
        raise exception 'No organisations exist; create an organisation first';
    end if;

    -- 1. generate a unique public token (UUID)
    v_token := gen_random_uuid();

    -- 2. generate a random private secret (32 bytes → 43 base64url chars)
    v_secret := core.gen_token(32);

    -- 3. insert the new automated service.
    --    the trg_hash_automated_service_secret trigger will automatically hash
    --    v_secret before it hits the disk.
    insert into core.automated_services (organisation, name, token, token_secret_hash, metadata, storage_limit, domain)
    values (v_org, p_name, v_token, v_secret, coalesce(p_metadata, '{}'::jsonb), p_storage_limit, p_domain)
    returning core.automated_services.id into v_id;

    -- 4. return the new automated service details.
    --    we return v_secret (the raw one) instead of a.token_secret_hash (the hashed one).
    return query
    select
        a.id,
        a.organisation,
        a.name,
        a.token,
        v_secret,   -- raw secret, only exposure ever
        a.status,
        a.metadata,
        a.created_at,
        a.updated_at,
        a.domain
    from core.automated_services a
    where a.id = v_id;
end;
$$;

-- FIX #2: Grant now correctly targets the 5-parameter signature that the
-- function body above actually implements. The previous grant referenced the
-- 3-parameter overload (varchar, jsonb, text), leaving the 5-parameter
-- version ungrantable and causing a permission error when p_storage_limit
-- or p_domain were supplied.
revoke all on function core.create_automated_service (varchar, jsonb, text, bigint, text)
from
  public;

grant
execute on function core.create_automated_service (varchar, jsonb, text, bigint, text) to sgrn_datastore,
sgrn_postgrest;

-- ============================================================
-- Backwards-compatible 2-argument wrapper
-- ============================================================
-- FIX #1: The previous wrapper declared only 3 return columns
-- (created_at, updated_at, domain) while the SELECT inside returned 10.
-- This caused a silent data-loss / runtime type mismatch.
-- The return type now matches the full-signature version exactly.
-- ============================================================
create or replace function core.create_automated_service (
  p_name varchar(128),
  p_metadata jsonb default '{}'
) returns table (
  id int,
  organisation text,
  name text,
  token uuid,
  token_secret text,
  status text,
  metadata jsonb,
  created_at timestamptz,
  updated_at timestamptz,
  domain text
) language sql security definer
set
  search_path = core,
  public as $$
    select *
    from core.create_automated_service(p_name, p_metadata, null::text, null::bigint, null::text);
$$;

revoke all on function core.create_automated_service (varchar, jsonb)
from
  public;

grant
execute on function core.create_automated_service (varchar, jsonb) to sgrn_datastore,
sgrn_postgrest;
