# HTTP API (sgrn)

This API is implemented natively in C++ using Drogon. All endpoints, filters, and serialization are custom-coded for high throughput and security. Endpoints are grouped by function: authentication, telemetry, storage, admin, query, GPAO, and PostgREST proxy. See the referenced files in this directory for details on each group.

Base path: `/api/v1/`

All JSON requests should send `Content-Type: application/json` unless explicitly stated otherwise.

> [!NOTE]
> This API is entirely natively built on top of the Drogon C++ web framework. Rather than utilizing pre-configured application servers, every endpoint, routing configuration, security filter, and JSON serialization parser you see documented below was strictly coded from scratch to guarantee high throughput and IT/OT safety.

## Authentication & Custom Filtering

There are two primary session paradigms, strictly gated by bespoke, natively-developed Drogon filters that intercept the controller paths:

- **Human Workflows:** Gated by the `UserAuthFilter`. This validates high-level React dashboard operators, validating multi-tenancy and extracting their internal role structures through custom, high-performance **Redis schema** caching logic.
- **Agent Workflows:** Gated by the `AgentAuthFilter`. This focuses solely on IoT data aggregation (`gateway`/automated services), providing strict validation for embedded M2M tokens without bloated web-session overhead.

Session propagation:

- Strictly uses standard `Authorization: Bearer <jwt>`, ensuring stateless validation scaled securely across node distributions.
- *Some legacy OT clients may fall back to the `SGRN-TOKEN` header, but modern tooling heavily prefers `Authorization`.*

## Endpoint Index

- [auth.md](auth.md): user sign-in/out, agent (automated service) sign-in/out and sessions.
- [services_api.md](services_api.md): the northbound automated-service interface (agent auth, storage, telemetry).
- [storage.md](storage.md): storage VFS, upload/download and drive operations.
- [admin.md](admin.md): admin endpoints (users, agents, endpoint list).
- [query.md](query.md): query endpoints for organisations/departments/statuses/user info.
- [gpao.md](gpao.md): production management (GPAO) CRUD endpoints.
