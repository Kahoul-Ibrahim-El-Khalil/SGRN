-- ============================================================
-- core.rotate_automated_service_credentials(automated_service_id, rotate_token)
-- ============================================================
-- Rotates an automated service's token_secret (always) and optionally rotates the public
-- token too (rotate_token=true).
--
-- Returns the raw (unhashed) new token_secret. This is the only time the new
-- secret is exposed in plain text.
-- ============================================================
create or replace function core.rotate_automated_service_credentials (
  p_automated_service_id int,
  p_rotate_token boolean default false
) returns table (
  id int,
  organisation text,
  name varchar(128),
  token uuid,
  token_secret text,
  status text,
  metadata jsonb,
  created_at timestamptz,
  updated_at timestamptz
) language plpgsql security definer
set
  search_path = core,
  public as $$
declare
    v_token  uuid;
    v_secret text;
    v_row    core.automated_services%rowtype;
    v_tries  int := 0;
begin
    if p_automated_service_id is null or p_automated_service_id <= 0 then
        raise exception 'Invalid automated_service_id';
    end if;

    -- Ensure automated service exists first (and capture current row).
    select * into v_row from core.automated_services where core.automated_services.id = p_automated_service_id;
    if not found then
        raise exception 'Unknown automated_service_id %', p_automated_service_id;
    end if;

    v_secret := core.gen_token(32);

    if p_rotate_token then
        -- Generate a unique UUID
        loop
            v_tries := v_tries + 1;
            v_token := gen_random_uuid();
            begin
                update core.automated_services
                set token = v_token,
                    token_secret_hash = v_secret
                where id = p_automated_service_id;
                exit;
            exception when unique_violation then
                if v_tries >= 5 then
                    raise exception 'Failed to generate unique token after % attempts', v_tries;
                end if;
            end;
        end loop;
    else
        v_token := v_row.token;
        update core.automated_services
        set token_secret_hash = v_secret
        where id = p_automated_service_id;
    end if;

    -- Return the updated automated service record + raw secret.
    return query
    select
        a.id,
        a.organisation,
        a.name,
        a.token,
        v_secret,  -- raw secret, only exposure ever for this rotation
        a.status,
        a.metadata,
        a.created_at,
        a.updated_at
    from core.automated_services a
    where a.id = p_automated_service_id;
end;
$$;

revoke all on function core.rotate_automated_service_credentials (int, boolean)
from
  public;

grant
execute on function core.rotate_automated_service_credentials (int, boolean) to sgrn_datastore,
sgrn_postgrest;

-- Convenience wrapper when you know only the public token.
create or replace function core.rotate_automated_service_credentials_by_token (
  p_token uuid,
  p_rotate_token boolean default false
) returns table (
  id int,
  organisation text,
  name varchar(128),
  token uuid,
  token_secret text,
  status text,
  metadata jsonb,
  created_at timestamptz,
  updated_at timestamptz
) language plpgsql security definer
set
  search_path = core,
  public as $$
declare
    v_id int;
begin
    if p_token is null then
        raise exception 'Invalid token';
    end if;

    select id into v_id from core.automated_services where token = p_token;
    if not found then
        raise exception 'Unknown automated service token';
    end if;

    return query select * from core.rotate_automated_service_credentials(v_id, p_rotate_token);
end;
$$;

revoke all on function core.rotate_automated_service_credentials_by_token (uuid, boolean)
from
  public;

grant
execute on function core.rotate_automated_service_credentials_by_token (uuid, boolean) to sgrn_datastore,
sgrn_postgrest;
