## Rust JSG string boundary aborts the process on C++ exceptions (uncaught `JsExceptionThrown` / cxx UTF-16 error)

### Summary

Any `node-internal:url` method backed by the Rust `UrlUtil` (enabled by the
`NODEJS_URL_RUST` autogate) **crashes the process (SIGABRT, core dumped)** when
an argument either (a) fails JS `ToString`/`ToPrimitive` coercion, or (b) is a
string containing an unpaired UTF-16 surrogate. Both are trivially reachable
from ordinary Worker code. The C++ `UrlUtil` handles both cases as normal,
catchable JS `TypeError`s.

Root cause is not specific to URL code: it is a general defect in the Rust JSG
method-callback boundary. Every Rust `#[jsg_method]` taking a `String` argument
is affected.

### Impact

- Deterministic worker process abort (denial of service) on attacker- or
  even accidentally-supplied input.
- Reached in production via the public wrappers and internal callers, e.g.:
  - `url.domainToASCII('\uD800')`
  - `new URL(...)` / legacy `url.parse(...)` whose host carries an unpaired
    surrogate, which flows to `urlUtil.toASCII(this.hostname)`
    (`src/node/internal/legacy_url.ts:429`) and
    `urlUtil.format(urlObject.href, ...)` (`:1009`).

### Root cause

`unwrap_string` throws a C++ exception:

- `src/rust/jsg/ffi.c++:646-654` — `jsg::check(value->ToString(...))` throws
  `workerd::jsg::JsExceptionThrown` when coercion fails; and the
  `rust::String(const char16_t*, len)` construction from `view.data16()` throws
  `std::invalid_argument: data for rust::String is not utf-16` for unpaired
  surrogates.

It is declared non-fallible in the bridge, so CXX does not translate the throw:

- `src/rust/jsg/v8.rs:509` — `pub unsafe fn unwrap_string(...) -> String;`
  (no `Result`).

V8 invokes the `jsg-macros` `extern "C"` callback directly, with no C++
`TryCatch`:

- `src/rust/jsg/ffi.c++:918-924` — method `FunctionTemplate`s point straight at
  the Rust callback pointer.

The only guard is `catch_panic`, which catches Rust panics only:

- `src/rust/jsg/lib.rs:883-899` — `std::panic::catch_unwind(...)` does not catch
  foreign (C++) exceptions.

Result: the C++ exception unwinds through the `nounwind` Rust `extern "C"` frame
→ `libc++abi` `terminate` → `abort()`.

By contrast, C++ JSG wraps every callback in `TryCatch`/`liftKj`, converting
`JsExceptionThrown` into a JS exception — which is why the C++ `UrlUtil` never
crashed.

### Reproduction

Under a production-representative build (`-c opt`, `NDEBUG`):

```
u.domainToASCII(Object.create(null))   // JsExceptionThrown  -> SIGABRT
u.toASCII('\uD800')                    // invalid_argument   -> SIGABRT
u.format('\uD800')                     // invalid_argument   -> SIGABRT
```

Observed: `libc++abi: terminating due to uncaught exception of type
workerd::jsg::JsExceptionThrown` and `... std::invalid_argument: data for
rust::String is not utf-16`, exit signal 6 (core dumped).

A crash-resilient fuzz driver over the JSG boundary confirms the same two
classes crash all five methods (`domainToASCII`, `domainToUnicode`, `toASCII`,
`canonicalizeIp`, `format`):

| Class | Example input | Exception thrown |
| --- | --- | --- |
| A: coercion fails | `Object.create(null)`, `Symbol()`, throwing `toString`, hostile `Proxy` | `workerd::jsg::JsExceptionThrown` (from `jsg::check(->ToString())`) |
| B: lone UTF-16 surrogate string | `'\uD800'`, `'\uDC00'`, `'\uD800'.repeat(5000)` | `std::invalid_argument: data for rust::String is not utf-16` |

Class B is the production-reachable path: a lone surrogate is a valid JS string
that survives the public wrapper's `` `${domain}` `` coercion unchanged, and
also arrives via internal callers with parser-derived host strings.

Reproduction assets (all `#[ignore]`, in `src/rust/api/url.rs`):

- `fuzz_jsg_boundary_segfault` — drives all methods through the real V8 boundary;
  resumable via the `URL_FUZZ_START` env var so an external driver can enumerate
  every crashing input past each abort.
- `fuzz_aggregator_segfault` — direct-call fuzz of the ada `url_aggregator`
  mutation paths (ran clean; rules out ada as the cause).

Run:

```
bazel build //src/rust/api:api_test -c opt
URL_FUZZ_LOG=/tmp/url_fuzz_last.txt \
  ./bazel-bin/src/rust/api/api_test --ignored --exact \
  url::tests::fuzz_jsg_boundary_segfault --nocapture
```

### Not an `ada-url` bug

- The Rust crate `ada-url` 3.4.6 bundles ada C++ **3.4.4**, byte-identical to
  workerd's `@ada-url`; the C ABI headers are identical. No version/ABI skew.
- Direct fuzzing of the ada `url_aggregator` mutation paths (with
  `ADA_DEVELOPMENT_CHECKS=0` to match production) ran clean over millions of
  inputs, including multi-KB and 200k-char hosts.

### Suggested fixes

1. **Boundary catch (general fix):** wrap the `jsg-macros`-generated `extern "C"`
   callback body so C++ exceptions thrown across the FFI are caught and converted
   to a JS exception (mirroring C++ JSG `liftKj`/`TryCatch`), instead of relying
   solely on `catch_panic` for Rust panics. This closes the defect for every Rust
   JSG method, not just URL.
2. **Fallible unwrap:** declare the unwrap shims as `-> Result<...>` so CXX
   translates C++ exceptions into Rust `Err`, which the glue then throws as a JS
   exception. `unwrap_string` should also handle non-UTF-16 input without throwing
   `std::invalid_argument` (e.g. lossy conversion or an explicit `TypeError`).

### Regression test

Add a `nodejs`-compat wd-test asserting these throw catchable `TypeError`s
(not crash) under both gate states:

```
require('node:url').domainToASCII('\uD800');            // must not crash
require('node:url').domainToASCII(Object.create(null)); // must throw TypeError
```
