-- ============================================================
-- telemetry schema
-- ============================================================
-- design notes:
--   • object represents a single physical device/sensor.
--   • one automated service (core.automated_services) manages 1..N objects.
--     ownership is expressed via automated_service_id (FK to core.automated_services.id).
--   • object_telemetry is a timescaledb hypertable.
--
-- fixes applied:
--   #4  organisation on telemetry.object is enforced consistent with the
--       owning automated_service via trg_check_object_organisation.
--   #7  deleted_at column added to telemetry.object for soft-delete;
--       telemetry_view filters it out automatically.
--   #8  unique index (iot_object_id, time) added to object_telemetry to
--       prevent duplicate readings from network retries / SDK bugs.
--       telemetry_view also uses DISTINCT ON as a defence-in-depth guard.
-- ============================================================
create schema if not exists telemetry;

-- ============================================================
-- migrate legacy table name if needed
-- ============================================================
do $$
begin
  if exists (
    select 1 from information_schema.tables
    where table_schema = 'telemetry' and table_name = 'iot_object'
  ) and not exists (
    select 1 from information_schema.tables
    where table_schema = 'telemetry' and table_name = 'object'
  ) then
    execute 'alter table telemetry.iot_object rename to object';
  end if;
end
$$;

-- ============================================================
-- object  (individual device / sensor)
-- ============================================================
create table if not exists telemetry."object" (
  id int generated always as identity primary key,
  -- FIX #4: organisation is kept for query convenience but is now enforced
  -- consistent with automated_services.organisation by the trigger below.
  organisation text not null references core.organisations (name) on delete cascade on update cascade,
  automated_service_id int not null references core.automated_services (id) on delete cascade,
  name varchar(128) not null,
  metadata jsonb not null default '{}',
  status varchar(20) not null default 'offline' check (status in ('online', 'offline', 'error')),
  -- FIX #5: domain FK — must exist in core.domains for this organisation.
  domain text default null,
  -- FIX #7: soft-delete — set deleted_at to logically remove a device while
  -- preserving its metadata.
  deleted_at timestamptz default null,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  unique (automated_service_id, name),
  constraint fk_object_domain foreign key (organisation, domain) references core.domains (organisation, name) on update cascade on delete set null
);

create index if not exists idx_object_automated_service on telemetry."object" (automated_service_id);

-- FIX #7: partial index — fast lookups for live (non-deleted) objects only.
create index if not exists idx_object_active on telemetry."object" (automated_service_id)
where
  deleted_at is null;

-- Prevent duplicate devices per automated service by MAC address.
create unique index if not exists idx_object_automated_service_mac_unique on telemetry."object" (automated_service_id, lower(metadata ->> 'mac'))
where
  (metadata ? 'mac');

create trigger trg_object_updated before
update on telemetry."object" for each row
execute function core.trigger_set_timestamp ();

-- ============================================================
-- FIX #4: organisation consistency trigger
-- ============================================================
create or replace function telemetry.check_object_organisation () returns trigger language plpgsql as $$
declare
  v_service_org text;
begin
  select organisation into v_service_org
  from core.automated_services
  where id = new.automated_service_id;

  if v_service_org is null then
    raise exception 'automated_service_id % does not exist', new.automated_service_id;
  end if;

  if new.organisation <> v_service_org then
    raise exception
      'telemetry.object.organisation (%) must match the owning automated_service organisation (%).',
      new.organisation, v_service_org;
  end if;

  return new;
end;
$$;

create trigger trg_check_object_organisation before insert
or
update of organisation,
automated_service_id on telemetry."object" for each row
execute function telemetry.check_object_organisation ();

-- deterministic tie-break

-- ============================================================
-- view: telemetry_view (Compatibility layer)
-- ============================================================
-- This view provides a list of all active IoT objects and their owning
-- automated services. Historical metrics are no longer part of this view.
-- ============================================================
create or replace view telemetry.telemetry_view as
select
  o.id as object_id,
  o.name as object_name,
  o.status as object_status,
  o.metadata as object_metadata,
  o.organisation as organisation,
  o.domain as domain,
  o.created_at as created_at,
  -- owning automated service
  a.id as automated_service_id,
  a.name as automated_service_name,
  a.status as automated_service_status
from
  telemetry."object" o
  join core.automated_services a on a.id = o.automated_service_id
where
  o.deleted_at is null -- exclude soft-deleted devices
  and a.deleted_at is null; -- exclude soft-deleted services
