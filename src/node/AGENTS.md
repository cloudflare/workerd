# src/node/ — Node.js Compatibility (TypeScript)

## OVERVIEW

TypeScript layer implementing Node.js built-in modules for Workers. ~70 top-level `.ts` files (one per `node:*` module) re-export from `internal/` implementations. No index.ts — C++ side registers each module. Tests live in `src/workerd/api/node/tests/`, not here. See `README.md` for 12 policy rules governing compat scope.

## ARCHITECTURE

Three-tier module system:

1. **C++ JSG modules** (`src/workerd/api/node/`) expose native ops via `node-internal:*` specifiers
2. **`internal/*.d.ts`** declare shapes the C++ modules provide (e.g., `crypto.d.ts`, `buffer.d.ts`)
3. **`internal/*.ts`** build full API surface; **top-level `*.ts`** re-export public API

Import specifiers:

- `node:buffer` → `src/node/buffer.ts` (public, user-importable)
- `node-internal:internal_buffer` → `src/node/internal/internal_buffer.ts` (private)
- `node-internal:crypto` → C++ JSG module declared by `internal/crypto.d.ts`
- `cloudflare-internal:*` → runtime-provided APIs (filesystem, sockets, http, messagechannel, workers)

Build: single `wd_ts_bundle` rule in `BUILD.bazel`; `modules` = public, `internal_modules` = private.

## MODULE GATING

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
