# src/node/ — Node.js Compatibility (TypeScript)

## OVERVIEW

TypeScript and JavaScript layer implementing Node.js compatible built-in modules for Workers.

It is split across multiple layers:

1. An **internal layer** consisting of:
  - Non-user-importable TypeScript and JavaScript files in `internal/` that implement core logic, utilities, and C++ JSG module declarations.
  - C++ JSG modules  (`src/workerd/api/node/`) expose native ops via `node-internal:*` specifiers
  - Some native internal modules may be implemented in Rust
2. A **public layer** of TypeScript files at the top-level that are user-importable.

It is common, but not required, for top-level `.ts` files to re-export from `internal/` via `node-internal:` specifiers. This allows for a clean separation between public API surface and internal implementation details.

See `README.md` for 12 policy rules governing compat scope and philosophy.

## MODULE GATING

`node:*` modules are gated behind the `nodejs_compat` compatibility flag.

The `node:async_hooks` module can be enabled individually via the `nodejs_als` compatibility flag.

Runtime compat flags checked via `Cloudflare.compatibilityFlags['flag_name']`:

| Flag                                   | Guards                                                                |
| -------------------------------------- | --------------------------------------------------------------------- |
| `enable_nodejs_http_server_modules`    | http Server/createServer, https Server                                |
| `nodejs_zlib`                          | zlib streaming classes (Deflate, Gzip, etc.)                          |
| `enable_nodejs_process_v2`             | Extended process/events functionality                                 |
| `remove_nodejs_compat_eol_v22/v23/v24` | EOL deprecation of specific API surfaces (crypto, util, tls, process) |

Most modules require `nodejs_compat` + `nodejs_compat_v2` flags (enforced by C++ side, not visible here).

## CONVENTIONS

- **`internal/*.js`** (not `.ts`) = upstream Node.js ports (streams_readable.js, streams_writable.js, etc.); paired with `.d.ts` type declarations
- **`internal/*.d.ts`** without matching `.ts` = declares C++ JSG module shape (crypto.d.ts → `node-internal:crypto`)
- **`internal/*.d.ts`** with matching `.js` = types for ported JS (streams_readable.d.ts → streams_readable.js)
- Top-level files are thin: import from `node-internal:*`, re-export with `export { ... }` / `export * from`
- Feature-gated exports use `if (!flag) { throw ... }` or conditional class assignment patterns
- Shared validators in `internal/validators.ts`; shared errors in `internal/internal_errors.ts`
- `_` prefix files (e.g., `_http_agent.ts`, `_stream_readable.ts`) = Node.js legacy internal module aliases
- Some Node.js compat APIs are non-functional stubs that are either non-ops or throw when called
