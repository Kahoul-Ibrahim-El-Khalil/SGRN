-- organisation details view
create or replace view core.organisation_details as
select
  id,
  name,
  description
from
  core.organisations;
