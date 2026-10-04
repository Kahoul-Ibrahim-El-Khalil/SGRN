# Repository actions

Small command line entry points for common maintenance tasks. Run them from the repository root with the SGRN development environment active:

- `python3 actions/db-init.py` initializes the local database.
- `python3 actions/db-reinit.py` removes and recreates the local database data directory.
- `python3 actions/orm-gen.py` regenerates datastore ORM models.
- `python3 actions/web-deploy.py` builds and deploys the web applications.
- `python3 actions/config-sync.py` syncs configuration; pass `--systemd` to also deploy systemd services.

The reusable implementations remain in `scripts/` (`scripts/db.py`, `scripts/web.py`, and `scripts/deploy.py`).
