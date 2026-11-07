-- ============================================================
-- core.audit_logs: Platform Audit Trail Logging (Tamper-Evident)
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
    created_at timestamptz not null default now(),
    seq bigint not null,
    prev_hash text not null,
    row_hash text not null
);

create unique index if not exists idx_audit_logs_org_seq on core.audit_logs (organisation, seq);
create index if not exists idx_audit_logs_row_hash on core.audit_logs (row_hash);
create index if not exists idx_audit_logs_org_date on core.audit_logs (organisation, created_at desc);
create index if not exists idx_audit_logs_action on core.audit_logs (action);
create index if not exists idx_audit_logs_actor on core.audit_logs (actor_type, actor_id);

-- Checkpoints table for periodic Ed25519 signed snapshots
create table if not exists core.audit_checkpoints (
    id bigserial primary key,
    organisation text not null references core.organisations(name) on delete cascade,
    last_seq bigint not null,
    last_hash text not null,
    sink_target text,
    signature text,
    public_key text,
    created_at timestamptz not null default now()
);

create index if not exists idx_audit_checkpoints_org_seq on core.audit_checkpoints (organisation, last_seq desc);

-- Append function: executes under advisory lock per-organisation
create or replace function core.append_audit_log(
    p_organisation text,
    p_actor_type text,
    p_actor_id int,
    p_actor_name text,
    p_action text,
    p_target_type text default null,
    p_target_id text default null,
    p_ip inet default '127.0.0.1'::inet,
    p_details jsonb default '{}'::jsonb,
    p_status text default 'success',
    p_created_at timestamptz default now()
)
returns table (
    out_id bigint,
    out_seq bigint,
    out_prev_hash text,
    out_row_hash text,
    out_created_at timestamptz
)
language plpgsql
security definer
as $$
declare
    v_lock_key bigint;
    v_last_seq bigint;
    v_prev_hash text;
    v_canon text;
    v_row_hash text;
    v_new_id bigint;
    v_timestamp timestamptz;
begin
    v_lock_key := ('x' || substr(md5('sgrn_audit_lock_' || p_organisation), 1, 16))::bit(64)::bigint;
    perform pg_advisory_xact_lock(v_lock_key);

    select seq, row_hash
    into v_last_seq, v_prev_hash
    from core.audit_logs
    where organisation = p_organisation
    order by seq desc
    limit 1;

    if v_last_seq is null then
        v_last_seq := 1;
        v_prev_hash := '0000000000000000000000000000000000000000000000000000000000000000';
    else
        v_last_seq := v_last_seq + 1;
    end if;

    v_timestamp := coalesce(p_created_at, now());

    v_canon := v_prev_hash || '|' ||
               v_last_seq || '|' ||
               p_organisation || '|' ||
               p_actor_type || '|' ||
               coalesce(p_actor_id::text, '') || '|' ||
               p_actor_name || '|' ||
               p_action || '|' ||
               coalesce(p_target_type, '') || '|' ||
               coalesce(p_target_id, '') || '|' ||
               coalesce(p_ip::text, '') || '|' ||
               coalesce((p_details)::text, '{}') || '|' ||
               p_status || '|' ||
               to_char(v_timestamp at time zone 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"');

    v_row_hash := encode(sha256(v_canon::bytea), 'hex');

    insert into core.audit_logs (
        organisation, actor_type, actor_id, actor_name, action, target_type, target_id,
        ip, details, status, created_at, seq, prev_hash, row_hash
    ) values (
        p_organisation, p_actor_type, p_actor_id, p_actor_name, p_action, p_target_type, p_target_id,
        p_ip, coalesce(p_details, '{}'::jsonb), p_status, v_timestamp, v_last_seq, v_prev_hash, v_row_hash
    )
    returning id into v_new_id;

    return query select v_new_id, v_last_seq, v_prev_hash, v_row_hash, v_timestamp;
end;
$$;

-- Verification function walking the chain for an organisation
create or replace function core.verify_audit_chain(p_organisation text)
returns table (
    is_valid boolean,
    total_rows bigint,
    first_broken_seq bigint,
    first_broken_id bigint,
    failure_reason text
)
language plpgsql
stable
as $$
declare
    r record;
    v_min_seq bigint;
    v_expected_seq bigint := 1;
    v_expected_prev text := '0000000000000000000000000000000000000000000000000000000000000000';
    v_canon text;
    v_calc_hash text;
    v_count bigint := 0;
    v_cp record;
begin
    select min(seq) into v_min_seq from core.audit_logs where organisation = p_organisation;
    if v_min_seq is null then
        return query select true, 0::bigint, null::bigint, null::bigint, 'Chain empty'::text;
        return;
    end if;

    if v_min_seq > 1 then
        select last_seq, last_hash into v_cp
        from core.audit_checkpoints
        where organisation = p_organisation
          and last_seq = v_min_seq - 1
        order by created_at desc
        limit 1;

        if not found then
            return query select false, 0::bigint, v_min_seq, null::bigint,
                ('Missing signed checkpoint for purged segment prior to seq ' || v_min_seq)::text;
            return;
        end if;

        v_expected_seq := v_min_seq;
        v_expected_prev := v_cp.last_hash;
    end if;

    for r in
        select id, organisation, actor_type, actor_id, actor_name, action, target_type, target_id,
               ip, details, status, created_at, seq, prev_hash, row_hash
        from core.audit_logs
        where organisation = p_organisation
        order by seq asc, id asc
    loop
        v_count := v_count + 1;

        if r.seq <> v_expected_seq then
            return query select false, v_count, r.seq, r.id,
                ('Sequence discontinuity: expected ' || v_expected_seq || ' but got ' || r.seq)::text;
            return;
        end if;

        if r.prev_hash <> v_expected_prev then
            return query select false, v_count, r.seq, r.id,
                ('Prev hash mismatch: expected ' || v_expected_prev || ' but got ' || r.prev_hash)::text;
            return;
        end if;

        v_canon := r.prev_hash || '|' ||
                   r.seq || '|' ||
                   r.organisation || '|' ||
                   r.actor_type || '|' ||
                   coalesce(r.actor_id::text, '') || '|' ||
                   r.actor_name || '|' ||
                   r.action || '|' ||
                   coalesce(r.target_type, '') || '|' ||
                   coalesce(r.target_id, '') || '|' ||
                   coalesce(r.ip::text, '') || '|' ||
                   coalesce((r.details)::text, '{}') || '|' ||
                   r.status || '|' ||
                   to_char(r.created_at at time zone 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"');

        v_calc_hash := encode(sha256(v_canon::bytea), 'hex');

        if r.row_hash <> v_calc_hash then
            return query select false, v_count, r.seq, r.id,
                ('Row hash mismatch: record tampered. expected ' || v_calc_hash || ' but got ' || r.row_hash)::text;
            return;
        end if;

        v_expected_prev := r.row_hash;
        v_expected_seq := v_expected_seq + 1;
    end loop;

    return query select true, v_count, null::bigint, null::bigint, 'Chain intact'::text;
end;
$$;

-- Trigger preventing UPDATE, DELETE on audit_logs
create or replace function core.trg_prohibit_audit_tampering()
returns trigger
language plpgsql
as $$
begin
    if TG_OP = 'DELETE' and current_setting('sgrn.allow_audit_purge', true) = 'on' then
        return old;
    end if;
    raise exception 'Audit logs are tamper-evident and append-only. UPDATE and DELETE operations are prohibited.';
end;
$$;

drop trigger if exists trg_audit_tamper_guard on core.audit_logs;
create trigger trg_audit_tamper_guard
    before update or delete on core.audit_logs
    for each row
    execute function core.trg_prohibit_audit_tampering();

-- Controlled audit retention purge function
create or replace function core.purge_audit_logs(
    p_organisation text,
    p_older_than_interval interval
)
returns table (
    purged_count bigint,
    purged_up_to_seq bigint,
    status_message text
)
language plpgsql
security definer
set search_path = core, pg_catalog, public
as $$
declare
    v_lock_key bigint;
    v_checkpoint record;
    v_count bigint := 0;
begin
    if p_older_than_interval < interval '90 days' then
        raise exception 'Audit log retention policy requires at least 90 days retention';
    end if;

    -- Per-organisation advisory transaction lock
    v_lock_key := ('x' || substr(md5('sgrn_audit_purge_' || p_organisation), 1, 15))::bit(64)::bigint;
    perform pg_advisory_xact_lock(v_lock_key);

    -- Find most recent signed checkpoint older than the retention threshold
    select id, last_seq, last_hash into v_checkpoint
    from core.audit_checkpoints
    where organisation = p_organisation
      and created_at <= now() - p_older_than_interval
    order by last_seq desc
    limit 1;

    if not found then
        return query select 0::bigint, 0::bigint,
            ('No verified checkpoint older than ' || p_older_than_interval::text || ' found. Purge refused to preserve chain continuity.')::text;
        return;
    end if;

    -- Enable bypass in current transaction for the tamper guard trigger
    perform set_config('sgrn.allow_audit_purge', 'on', true);

    delete from core.audit_logs
    where organisation = p_organisation
      and seq <= v_checkpoint.last_seq;

    get diagnostics v_count = row_count;

    return query select v_count, v_checkpoint.last_seq,
        ('Successfully purged ' || v_count || ' records up to checkpoint seq ' || v_checkpoint.last_seq)::text;
end;
$$;

grant execute on function core.append_audit_log to public;
grant execute on function core.verify_audit_chain to public;
grant execute on function core.purge_audit_logs to public;
grant select on core.audit_checkpoints to public;
