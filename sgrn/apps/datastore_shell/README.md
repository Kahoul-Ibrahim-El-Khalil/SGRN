# datastore-shell

Unix-like interactive shell and AngelScript administration engine for the SGRN datastore API: navigate remote storage with `ls`/`cd`, move bytes with `get`/`put`/`cat`, manage entries with `mkdir`/`mv`/`rm`, perform full administrative tasks, and pipe remote storage data through local external processes (`zstd`, `gzip`, `jq`, `grep`, `wc`) directly into AngelScript variables.

## Role

Dual mode & AngelScript engine (mirrors `s7shell`):
1. **REPL mode**: Drop into a readline REPL with persistent history (`~/.datastore_shell_history`), live tab completion, external process wrapper syntax (`!(cmd)`), and variable redirection (`> variable`).
2. **Script execution mode**: Run AngelScript batch files (`datastore_shell script.as`).
3. **One-shot command mode**: Execute individual administrative commands and exit with a status code (`datastore_shell --url URL --token T ls /`).

## Process Piping & Stream Compression (`!(cmd)`)

Stream remote datastore files through local external processes (`zstd`, `gzip`, `jq`, `grep`, `wc`) directly in the shell using `!(cmd)` notation, and redirect output into AngelScript variables or local files:

### 1. REPL Piping to AngelScript Variables (`> variable`)
```text
# Decompress remote file using external zstd and store output in variable 'decompressed':
dss [users] /$ cat /admin@local.com/demo.py.zst > !(zstd -d) > decompressed
Assigned 1450 bytes to variable 'decompressed'

# Print variable content in REPL:
dss [users] /$ print(decompressed);

# Leading variable assignment syntax:
dss [users] /$ string decompressed = cat /admin@local.com/demo.py.zst | !(zstd -d)
Assigned 1450 bytes to variable 'decompressed'

# Pipe remote log file through complex external pipeline into variable 'err_count':
dss [users] /$ cat /admin@local.com/app.log > !(grep ERROR | wc -l) > err_count
Assigned 4 bytes to variable 'err_count'
```

### 2. REPL Piping to Files or Standard Output
```text
# Download remote file and compress with zstd locally into a file:
dss [users] /$ cat /admin@local.com/demo.py | !(zstd -c) > demo.py.zst

# Pipe remote JSON through local jq:
dss [users] /$ cat /admin@local.com/config.json | !(jq .)
```

### 3. AngelScript Programmatic Piping (`script.as` or REPL)
```cpp
void main() {
    // 1. Download remote file content into a string variable
    string content = cat("/admin@local.com/demo.py.zst");

    // 2. Pipe variable bytes into local zstd process to decompress
    string decompressed = pipe(content, "zstd -d");
    print("Compressed size: " + content.length() + " bytes\n");
    print("Decompressed size: " + decompressed.length() + " bytes\n");

    // 3. Pipe variable through any UNIX shell pipeline
    string lines = pipe(decompressed, "grep import | wc -l");
    print("Import statement count: " + lines);
}
```

## AngelScript Engine & Administrative Functions

| AngelScript Function | Description |
|----------------------|-------------|
| `cat(path) -> string` | Download remote file content into a string variable |
| `pipe(data, shell_cmd) -> string` | Feed string/binary data to standard input of `shell_cmd` and return stdout |
| `exec(shell_cmd, data) -> string` | Execute local shell command with optional stdin input and return stdout |
| `createUser(first, family, email, pw, org, status)` | Register a user account (admin) |
| `listUsers()` | List registered users |
| `createService(name, org) -> string` | Register an automated service (admin) |
| `listServices()` | List registered automated services |
| `rotateServiceToken(nameOrId) -> string` | Rotate service token (admin) |
| `listOrgs()` | List organisations |
| `listDomains(org)` | List organisation domains |
| `listStatuses(org)` | List organisation statuses |
| `whoami()` | Print active user info as JSON |
| `storageStats()` | Print storage stats as JSON |
| `storageInfo()` | Print storage constraints as JSON |
| `setScope(scopeName)` | Set storage scope (`personal`, `domain`, `users`, etc.) |
| `getScope()` | Return active storage scope |

## Tab Completion

The shell features context-aware **Tab Completion**:
* **Top Level**: Completes commands (`connect`, `login`, `cat`, `useradd`, `scope`, etc.) and AngelScript functions (`createUser`, `setScope`, `pipe`, `cat`).
* **Scope Names**: Completes `scope` arguments (`auto`, `personal`, `users`, `automated-services`, `domain`).
* **Remote Pathing**: Queries the backend drive API live as you type to auto-complete remote directories and files for `cd`, `ls`, `cat`, `mkdir`, `rm`, `tree`, `du`, `zip`, `get`, `put`.

## Authentication: log in as yourself

```text
dss (offline)$ login --email admin@example.com
password:
Connected to https://localhost:8443 as user.
```

`connect` accepts all credential shapes and picks user > session-token > service.

## Dependencies

Links `sgrn::core` (AngelScriptEngine base, string/array/dict add-ons, filesystem module), `sgrn_sdk` (REST transport, session, drive API), `sgrn_utils` and fmt; readline/history via `cmake/find_readline.cmake`.
