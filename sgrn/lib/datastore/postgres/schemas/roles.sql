-- ============================================================
-- core.roles & core.user_roles: Dynamic RBAC & Custom Scopes
-- ============================================================
create table if not exists core.roles (
    id serial primary key,
    organisation text not null references core.organisations(name) on delete cascade on update cascade,
    name varchar(64) not null,
    description text,
    permissions jsonb not null default '[]',
    is_system boolean not null default false,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now(),
    unique (organisation, name)
);

create table if not exists core.user_roles (
    id bigserial primary key,
    user_id int references core.users(id) on delete cascade,
    automated_service_id int references core.automated_services(id) on delete cascade,
    role_id int not null references core.roles(id) on delete cascade,
    created_at timestamptz not null default now(),
    constraint chk_user_or_service check (
        (user_id is not null and automated_service_id is null) or
        (user_id is null and automated_service_id is not null)
    )
);

create index if not exists idx_user_roles_user on core.user_roles (user_id) where user_id is not null;
create index if not exists idx_user_roles_service on core.user_roles (automated_service_id) where automated_service_id is not null;
