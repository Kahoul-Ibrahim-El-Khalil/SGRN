-- ============================================================
-- core.authenticate(email, password)
-- ============================================================
-- returns the full user_details row if credentials are valid,
-- empty set otherwise. the password check happens entirely
-- inside the db — the hash is never exposed to the caller.
--
-- security definer lets sgrn_datastore call this without having
-- direct select on core.users (which holds the hash column).
--
-- schema change: core.roles table removed. core.users.role is
-- now a text column — user_details exposes it directly as `role`.
-- the c++ service reads row["role"] instead of row["role_name"].
-- ============================================================
create or replace function core.authenticate_user (t_email text, t_password text) returns setof core.user_details language sql stable security definer
set
  search_path = core,
  public -- pin search_path to prevent hijacking
  as $$
    select ud.*
    from   core.user_details ud
    join   core.users        u  on u.id = ud.id
    where  u.email     = t_email
      and  u.is_active = true
      and  u.password  = crypt(t_password, u.password);
$$;

-- only the api role may call this function — never public
revoke all on function core.authenticate_user (text, text)
from
  public;

grant
execute on function core.authenticate_user (text, text) to sgrn_datastore,
sgrn_postgrest;
