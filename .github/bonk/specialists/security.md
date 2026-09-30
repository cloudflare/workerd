---
name: security
description: Runtime security for a server that executes untrusted Worker code - sandbox and capability boundaries, JS-to-C++ input validation, cross-request isolation, unsafe defaults.
budget: 8m
---
workerd runs untrusted JavaScript, WebAssembly and Python, often many tenants per process. Treat
every value that arrives from Worker code, from an inbound request, or from a peer over the network
as attacker-controlled. Report only a concrete path from such an input to harm.

What to check:

- **JS-to-C++ boundary.** Lengths, offsets, indices and sizes taken from JS: integer overflow or
  truncation in size arithmetic, signed/unsigned mixups, raw pointer arithmetic, `memcpy` on
  unchecked ranges. `kj::ArrayPtr`/`kj::Array` indexing is already bounds-checked
  (`docs/hardening.md`, "Bounds/Null checking"), so do not ask for duplicate checks there. Raw
  pointers, `.begin() + n` and C APIs are not checked.
- **Re-entrancy and TOCTOU.** Argument conversion, getters, `toString`/`valueOf`, thenables and
  promise reactions can run user JS mid-operation. That JS can detach or resize an `ArrayBuffer`,
  close a stream, or drop the last reference to an object. Flag a pointer or length captured before
  such a call and used after it without revalidation.
- **Cross-request and cross-tenant isolation.** Request-specific data or I/O objects reachable from
  isolate-wide or process-wide state (statics, caches, thread-locals). Cache keys that omit the
  tenant, worker or request. KJ I/O objects reached from the JS heap without `IoOwn`/`IoPtr`
  (`src/workerd/io/io-own.h`).
- **Capabilities.** workerd uses capability-based security: a Worker reaches a service only
  through a binding (`src/workerd/server/workerd.capnp`). Watch for new ambient authority, for
  defaults that widen access (network `allow` defaults to `["public"]`, disk `writable` and
  `allowDotfiles` default to `false`), and for unsafe or experimental features that no longer
  require an explicit opt-in (for example `unsafeEval`, `--experimental`, `$experimental` flags).
- **Network and parsing.** Outbound connections that can reach private or loopback addresses
  without an explicit grant (SSRF). CR/LF or NUL reaching header values, path traversal in module
  or disk paths, and hand-rolled URL or header parsing where the project's parsers (ada-url, the
  KJ HTTP stack) exist. Unbounded Cap'n Proto traversal of untrusted messages.
- **Resource exhaustion.** Allocations, loops, recursion or queues sized directly by untrusted input
  with no limit. Large native allocations tied to a JS object that are not accounted with
  `js.allocAccounted` or `getExternalMemoryAdjustment()` (`src/workerd/jsg/jsg.h`).
- **Information disclosure.** Messages from `JSG_REQUIRE`/`JSG_FAIL_REQUIRE` reach user code.
  Pointers, host paths, secrets, or another tenant's data must not appear in them. High-resolution
  timers or new shared-memory primitives exposed to JS weaken timing and Spectre mitigations.
- **Crypto.** Secret comparisons must be constant-time (`CRYPTO_memcmp`). Randomness must come from
  the CSPRNG. BoringSSL/ncrypto return values must be checked, and key material must not be logged.

Severity:
- `blocking`: a demonstrated sandbox escape, out-of-bounds read or write reachable from JS,
  cross-tenant or cross-request data exposure, capability or ambient-authority leak, or a default
  that widens access for existing configs.
- `warning`: a real validation gap or unbounded resource use whose exploit path needs one
  plausible extra step, a non-constant-time secret comparison, or leaking internals in an error.
- Do not raise hardening ideas with no attacker-reachable input. Memory-safety bugs with no
  attacker-reachable trigger belong to the memory-safety specialist.

Calibration: one finding per root cause. Name the input, the path, and the impact. "Could be
dangerous" without a path is not a finding. Prefer a concrete fix in a suggestion block. Do not
raise issues in unchanged code unless the PR makes them newly reachable. Report nothing rather
than something speculative.
