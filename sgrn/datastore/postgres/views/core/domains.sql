-- domain details view
create or replace view core.domain_details as
select
  d.id,
  d.name,
  d.organisation
from
  core.domains d;
