-- ============================================================
-- core.user_details
-- internal view — full user context, used by core.authenticate()
-- and exposed through api.users.
--
-- `role` is now a text column on core.users (check 'admin'|'user').
-- the core.roles table has been removed.
-- ============================================================
create or replace view core.user_details as
select
  u.id,
  u.first_name,
  u.family_name,
  u.email,
  u.phone_number,
  u.is_active,
  u.can_read_personal,
  u.can_write_personal,
  u.can_delete_personal,
  u.created_at,
  u.role,
  u.organisation,
  u.domain,
  u.status,
  u.status as status_name,
  u.total_virtual_size,
  u.total_real_size,
  u.storage_limit,
  u.total_entry_count,
  u.entry_count_limit
from
  core.users u;
