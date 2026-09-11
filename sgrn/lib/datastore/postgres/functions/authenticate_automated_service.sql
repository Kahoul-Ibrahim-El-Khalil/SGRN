-- ============================================================
-- core.authenticate_automated_service(t_token, t_secret)
-- ============================================================
-- returns the automated service details if credentials are valid,
-- empty set otherwise. the secret check happens inside the db.
-- ============================================================
create or replace function core.authenticate_automated_service (t_token uuid, t_secret text) returns table (
  id int,
  organisation text,
  name text,
  token uuid,
  status text,
  metadata jsonb,
  total_virtual_size bigint,
  total_real_size bigint,
  storage_limit bigint,
  total_entry_count bigint,
  entry_count_limit bigint,
  created_at timestamptz,
  updated_at timestamptz,
  domain text
) language sql stable security definer
set
  search_path = core,
  public as $$
    select 
        a.id,
        a.organisation,
        a.name,
        a.token,
        a.status,
        a.metadata,
        a.total_virtual_size,
        a.total_real_size,
        a.storage_limit,
        a.total_entry_count,
        a.entry_count_limit,
        a.created_at,
        a.updated_at,
        a.domain
    from core.automated_services a
    where a.token = t_token
      and a.token_secret_hash = crypt(t_secret, a.token_secret_hash);
$$;

-- permissions
revoke all on function core.authenticate_automated_service (uuid, text)
from
  public;

grant
execute on function core.authenticate_automated_service (uuid, text) to sgrn_datastore,
sgrn_postgrest;
