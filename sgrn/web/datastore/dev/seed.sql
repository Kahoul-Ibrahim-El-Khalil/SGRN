-- ============================================================
-- sgrn/web/datastore/dev/seed.sql
-- Dev-only seed data for local UI development.
-- Safe to re-run: ON CONFLICT DO NOTHING throughout.
-- Run automatically by `bun run dev` via dev.ts after schema init.
-- Add any test users, orgs, or rows you need for UI work here.
-- ============================================================

-- Dev regular user
insert into core.users (organisation, first_name, family_name, email, password, role, status, is_active)
values (
  'CIC-Moulins Guelma',
  'Dev', 'User',
  'dev@local.com',
  crypt('devpassword', gen_salt('bf', 4)),
  'user', 'active', true
) on conflict (email) do nothing;

-- Dev admin user
insert into core.users (organisation, first_name, family_name, email, password, role, status, is_active)
values (
  'CIC-Moulins Guelma',
  'Dev', 'Admin',
  'dev-admin@local.com',
  crypt('devadminpassword', gen_salt('bf', 4)),
  'admin', 'active', true
) on conflict (email) do nothing;
