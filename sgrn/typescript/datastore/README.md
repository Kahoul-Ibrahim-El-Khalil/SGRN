# @sgrn/datastore

TypeScript bindings for the SGRN Datastore.

## Import

```ts
import {
  AutomatedServiceClient,
  DirectoryTree,
  isError,
  isSuccess,
  type Credentials,
} from "@sgrn/datastore";
```

## What it exposes

- `AutomatedServiceClient` for automated-service auth and file transfer
- `DirectoryTree` for filesystem traversal and mirroring
- Shared datastore DTOs and result helpers via `@sgrn/types`

## Notes

- The package is source-first and intended to be imported directly by Bun scripts and frontend tooling.
- It depends on the shared `@sgrn/types` package for common result and DTO definitions.
