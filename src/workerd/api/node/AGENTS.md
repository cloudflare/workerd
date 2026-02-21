# src/workerd/api/node/

C++ implementations of Node.js built-in modules. Each module = JSG-bound class registered via `NODEJS_MODULES(V)` macro in `node.h`. TypeScript counterpart lives in `src/node/`.

## BUILD TARGETS

- **`node-core`**: buffer, dns, i18n, sqlite, url. **No `//src/workerd/io` dep** — do not add one. Depends on Rust crates (`cxx-integration`, `dns`, `net`), `ada-url`, `nbytes`, `simdutf`.
- **`node`**: async-hooks, crypto, diagnostics-channel, module, process, timers, util, zlib-util. Depends on `io`, `ncrypto`, `zstd`, `kj-brotli`. Depends on `node-core`.
- **`exceptions`**: Standalone Node.js exception types; dep of `node-core`.

## MODULE REGISTRATION

1. Add `JSG_RESOURCE_TYPE` class in `<module>.h` + `<module>.c++`
2. Define `EW_NODE_<MODULE>_ISOLATE_TYPES` macro in header
3. Add `V(ClassName, "node-internal:<name>")` to `NODEJS_MODULES(V)` in `node.h` (or `NODEJS_MODULES_EXPERIMENTAL(V)` for staging)
4. Append to `EW_NODE_ISOLATE_TYPES` at bottom of `node.h`
5. Per-module compat flags gated in `registerNodeJsCompatModules()` — add `isNode*Module()` + `featureFlags.getEnable*()` check
6. DnsUtil special-cased: C++ fallback when `RUST_BACKED_NODE_DNS` autogate disabled

## TESTING

Tests in `tests/`. Naming: `<module>-test.js` + `<module>-test.wd-test`; `-nodejs-` infix when needing compat flags. All tests set `compatibilityFlags = ["nodejs_compat", "nodejs_compat_v2", "experimental"]`. Network tests (net, tls, http) use sidecar `js_binary` targets. `fixtures/` has 46 PEM files for crypto. `process-stdio` tests use `sh_test` with `.expected_stdout`/`.expected_stderr`. C++ unit test: `buffer-test.c++` via `kj_test`.
