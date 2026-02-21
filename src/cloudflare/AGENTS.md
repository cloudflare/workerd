# src/cloudflare/

## OVERVIEW

TypeScript implementations of Cloudflare product APIs (AI, D1, R2, Vectorize, etc.). Top-level `.ts` files re-export from `internal/` via `cloudflare-internal:` specifiers. `internal/*.d.ts` declare types for C++ runtime bindings; `internal/*.ts` contain actual logic.

## STRUCTURE

- `<product>.ts` — Public entry point; re-exports from `cloudflare-internal:<product>-api`
- `internal/<product>-api.ts` — Implementation; imports runtime types from sibling `.d.ts`
- `internal/*.d.ts` — Type declarations for C++ runtime-provided modules (no implementation)
- `internal/tracing-helpers.ts` — Shared instrumentation (`withSpan`); imported as `cloudflare-internal:tracing-helpers`
- `internal/test/<product>/` — Per-product test directory

## TEST PATTERN

Each product test directory contains:

| File                                    | Purpose                                                      |
| --------------------------------------- | ------------------------------------------------------------ |
| `<product>-api-test.js`                 | JS test; named exports with `test()` methods + `node:assert` |
| `<product>-api-test.wd-test`            | Cap'n Proto config wiring test worker to mock                |
| `<product>-mock.js`                     | Mock service simulating upstream API                         |
| `<product>-api-test.py`                 | Python variant (optional)                                    |
| `python-<product>-api-test.wd-test`     | Python test config (optional)                                |
| `<product>-api-instrumentation-test.js` | Tracing/instrumentation tests (optional)                     |

Mock wiring uses `wrapped` bindings: `moduleName = "cloudflare-internal:<product>-api"` with `innerBindings` pointing `fetcher` at the mock service. Shared `instrumentation-test-helper.js` lives in `internal/test/`.
