# SGRN CRUD Library — Manifest-Driven Code Generation

`sgrn/crud/` is a reusable, manifest-driven C++ CRUD code generator and Drogon HTTP runtime library for SGRN projects.

It translates explicit YAML API manifests into tenant-scoped, type-safe Drogon REST API handlers compiled directly into your application.

---

## Architecture Overview

```
sgrn/crud/
├── manifests/                 # ← Declarative YAML definitions
│   ├── core.users.yaml
│   ├── core.domains.yaml
│   ├── core.user_domain_permissions.yaml
│   └── storage.files.yaml
├── include/                   # ← Runtime headers
│   ├── CrudSpec.hpp           # Manifest metadata & route config structs
│   ├── CrudPolicy.hpp         # Access policy & scope enums
│   ├── CrudFieldSpec.hpp      # Field definitions & PostgREST filter bitmasks
│   ├── CrudViewEngine.hpp     # Generic async SQL builder & executor
│   ├── CrudViewHandler.hpp    # CRTP Drogon controller base
│   ├── FilterHookRegistry.hpp # Custom filter hook registry for admin overrides
│   ├── AuditLogger.hpp        # Mutation audit logger
│   └── Validators.hpp         # Field constraint & format validation
├── src/                       # ← Runtime implementation
│   ├── CrudViewEngine.cpp
│   ├── AuditLogger.cpp
│   ├── Validators.cpp
│   └── FilterHookRegistry.cpp
├── generator/                 # ← Python codegen engine
│   ├── generate_crud.py       # Main generator CLI
│   ├── schema_provider.py     # PostgreSQL introspection & offline fallback
│   ├── manifest_loader.py     # YAML loader & normalizer
│   ├── codegen.py             # C++ template generator engine
│   └── validator.py           # Manifest ↔ database schema sync validator
├── templates/                 # ← Code generation templates
│   ├── handler.gen.hpp.j2
│   ├── spec.gen.hpp.j2
│   └── registered_views.cpp.j2
├── cmake/
│   └── sgrn_crud_codegen.cmake# CMake integration targets
└── README.md                  # ← Documentation (this file)
```

---

## Manifest Format

Each YAML file in `manifests/` defines the CRUD surface for a single database table:

```yaml
metadata:
  version: "1.0"
  description: "User management API"
  generated_timestamp: null

table:
  schema: core
  name: users
  primary_key: id
  primary_key_type: int

tenant:
  column: organisation
  required: true
  server_bound: true            # Bound server-side from session (never client override)

policy:
  read_scope: org               # org | own | global | admin
  write_scope: own              # org | own | global | admin
  delete_scope: admin           # org | own | global | admin
  allowed_roles:
    - admin
    - user
    - service_account
  max_limit: 500
  soft_delete: true             # Automatically appends `deleted_at IS NULL`
  audit: true                   # Logs mutations via AuditLogger
  versioning: true              # Optimistic locking support
  batch_enabled: false

routes:
  list:
    path: /api/v1/users
    method: GET
    auth_filters:
      - UserAuthFilter
  get:
    path: /api/v1/users/{id}
    method: GET
    auth_filters:
      - UserAuthFilter
  create:
    path: /api/v1/users
    method: POST
    auth_filters:
      - UserAuthFilter
  update:
    path: /api/v1/users/{id}
    method: PATCH
    auth_filters:
      - UserAuthFilter
  delete:
    path: /api/v1/users/{id}
    method: DELETE
    auth_filters:
      - UserAuthFilter

fields:
  id:
    type: int
    readable: false
    writable: false

  organisation:
    type: text
    readable: false
    writable: false

  email:
    type: text
    readable: true
    writable: true
    filterable: true
    operators:
      - eq
      - like
      - in
    validation:
      format: email
      maxlen: 255
      unique: true
    nullable: false

admin_overrides:
  - name: UsersAdminView
    path: /api/v1/admin/users
    description: "Admin-only users endpoint with custom filter hook"
    filter_hook: UsersAdminViewFilter
```

---

## How to Add New Manifests

1. Create a new YAML manifest file in `sgrn/crud/manifests/<schema>.<table_name>.yaml`.
2. Define `table`, `tenant`, `policy`, `routes`, and `fields`.
3. Run the generator script:
   ```bash
   python3 sgrn/crud/generator/generate_crud.py
   ```
   Or invoke the CMake target:
   ```bash
   cmake --build . --target sgrn_crud_generate
   ```
4. Build the C++ target:
   ```bash
   cmake --build . --target sgrn_crud
   ```

---

## PostgREST Filter Syntax

Query parameter filtering follows standard PostgREST syntax: `column=operator.value`.

| Operator | Query Example | Generated SQL |
|---|---|---|
| `eq` | `email=eq.user@acme.com` | `"email" = $1` |
| `neq` | `role=neq.admin` | `"role" != $1` |
| `gt` | `id=gt.100` | `"id" > $1` |
| `gte` | `id=gte.100` | `"id" >= $1` |
| `lt` | `id=lt.500` | `"id" < $1` |
| `lte` | `id=lte.500` | `"id" <= $1` |
| `like` | `email=like.%25@acme.com` | `"email" LIKE $1` |
| `in` | `role=in.admin,user` | `"role" IN ($1, $2)` |

---

## Admin Overrides and Filter Hooks

For administrative views requiring special filters (such as bypassing soft-delete or matching regexes), specify an `admin_overrides` section in the manifest and register a C++ filter hook in `FilterHookRegistry`:

```cpp
#include <sgrn/crud/FilterHookRegistry.hpp>

void registerCustomHooks() {
    sgrn::crud::FilterHookRegistry::instance().registerHook(
        "UsersAdminViewFilter",
        [](const drogon::HttpRequestPtr& req, std::string& sql_where, std::vector<std::string>& params) {
            // Custom C++ filter logic
            if (req->getParameter("include_deleted") == "true") {
                // Remove soft delete condition
            }
        }
    );
}
```

---

## Building and Integrating

Include `sgrn_crud` in your project's `CMakeLists.txt`:

```cmake
add_subdirectory(sgrn/crud)
target_link_libraries(my_server PRIVATE sgrn::crud)
```

In your main Drogon server startup:

```cpp
#include <RegisteredViews.cpp>

int main() {
    // Register all generated CRUD handlers
    sgrn::crud::generated::registerAllCrudViews();
    
    drogon::app().run();
    return 0;
}
```
