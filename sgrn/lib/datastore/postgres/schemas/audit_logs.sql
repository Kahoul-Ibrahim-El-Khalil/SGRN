-- ============================================================
-- core.audit_logs: Platform Audit Trail Logging
-- ============================================================
create table if not exists core.audit_logs (
    id bigserial primary key,
    organisation text not null references core.organisations(name) on delete cascade,
    actor_type text not null check (actor_type in ('user', 'automated_service', 'system')),
    actor_id int,
    actor_name text not null,
    action varchar(128) not null,
    target_type varchar(64),
    target_id text,
    ip inet,
    details jsonb default '{}',
    status text not null check (status in ('success', 'failure')),
    created_at timestamptz not null default now()
);

create index if not exists idx_audit_logs_org_date on core.audit_logs (organisation, created_at desc);
create index if not exists idx_audit_logs_action on core.audit_logs (action);
create index if not exists idx_audit_logs_actor on core.audit_logs (actor_type, actor_id);
