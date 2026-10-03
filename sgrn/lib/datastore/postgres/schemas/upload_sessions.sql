-- ============================================================
-- storage.upload_sessions & storage.upload_chunks: Resumable Upload Tracking
-- ============================================================
create table if not exists storage.upload_sessions (
    id text primary key,
    user_id int references core.users(id) on delete cascade,
    automated_service_id int references core.automated_services(id) on delete cascade,
    domain text,
    target_path text not null,
    filename text not null,
    mime_type text not null default 'application/octet-stream',
    total_size bigint not null check (total_size > 0),
    chunk_size bigint not null check (chunk_size > 0),
    total_chunks int not null check (total_chunks > 0),
    uploaded_chunks_count int not null default 0,
    status text not null default 'active' check (status in ('active', 'completed', 'aborted')),
    -- Set on complete: SHA-256 hex of the original (pre-compression) bytes.
    -- Used as the content-addressable namespace for segment keys in Garage.
    file_hash text default null,
    -- Total compressed size across all segments (set on complete).
    compressed_size bigint default null,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now(),
    expires_at timestamptz not null default (now() + interval '24 hours'),
    constraint chk_upload_owner check (
        (user_id is not null and automated_service_id is null) or
        (user_id is null and automated_service_id is not null) or
        (domain is not null)
    )
);

-- Idempotent column additions for existing databases
alter table storage.upload_sessions add column if not exists file_hash text default null;
alter table storage.upload_sessions add column if not exists compressed_size bigint default null;

create table if not exists storage.upload_chunks (
    id bigserial primary key,
    upload_id text not null references storage.upload_sessions(id) on delete cascade,
    chunk_index int not null check (chunk_index >= 0),
    chunk_size bigint not null check (chunk_size > 0),
    checksum text,
    storage_key text not null,
    -- Garage object_id: set immediately when the segment is pushed to S3.
    -- NULL means the segment has been received but not yet stored as a Garage object
    -- (should not occur in normal operation after the pipeline migration).
    object_id bigint references storage.objects(id) on delete set null,
    -- Compressed size of this segment as stored in Garage.
    compressed_size bigint default null,
    created_at timestamptz not null default now(),
    unique (upload_id, chunk_index)
);

-- Idempotent column additions for existing databases
alter table storage.upload_chunks add column if not exists object_id bigint references storage.objects(id) on delete set null;
alter table storage.upload_chunks add column if not exists compressed_size bigint default null;

create index if not exists idx_upload_sessions_user on storage.upload_sessions (user_id) where user_id is not null;
create index if not exists idx_upload_sessions_status on storage.upload_sessions (status, expires_at);
create index if not exists idx_upload_chunks_upload on storage.upload_chunks (upload_id);

-- ============================================================
-- core.webhooks: System Webhook Subscriptions
-- ============================================================
create table if not exists core.webhooks (
    id serial primary key,
    organisation text not null references core.organisations(name) on delete cascade,
    url text not null,
    secret text,  -- nullable: no signature if not set
    events text[] not null default array['user.signin', 'user.signout', 'service.signin', 'service.signout']::text[],
    is_active boolean not null default true,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

create index if not exists idx_webhooks_org on core.webhooks (organisation) where is_active is true;

-- Migration: add 'service.signout' to existing webhook subscriptions that don't have it yet
update core.webhooks
set events = array_append(events, 'service.signout')
where not ('service.signout' = any(events));

-- Migration: make secret nullable (allows unsigned webhooks for internal consumers)
alter table core.webhooks alter column secret drop not null;
