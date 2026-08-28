# crypto.DigestStream

An informal specification of the Cloudflare-specific DigestStream (a
WritableStream subclass computing a hash digest) as implemented in workerd,
derived from — and kept in lockstep with — the test suite in this
directory. **The tests are the normative artifact.** Both the C++
implementation (`src/workerd/api/crypto/crypto.{h,c++}`) and the TypeScript
implementation (`src/per_isolate/crypto/digest-stream.ts`, behind
`typescript_implemented_streams`) are covered. Both drive the SAME native
digest context (`utils.createDigestContext` → CRC/OpenSSL contexts and the
WTF-8/toWellFormed string encoder), so hashing, string encoding, and byte
counting are parity by construction; divergences live in the stream
wrapper and promise plumbing. DigestStream is non-standard: no WPT exists.

## Interface

```webidl
[Exposed=Worker]
interface DigestStream : WritableStream {
  constructor((DOMString or HashAlgorithm) algorithm,
              optional DigestStreamOptions options = {});
  readonly attribute Promise<ArrayBuffer> digest;  // memoized identity
  readonly attribute bigint bytesWritten;
};
dictionary DigestStreamOptions { boolean toWellFormed = false; };
```

- Algorithms (case-insensitive over the whole name, string or `{name}`
  object): `md5`, `SHA-1`, `SHA-256`, `SHA-384`, `SHA-512`, `crc32`,
  `crc32c`, `crc64nvme`. Unknown names throw `NotSupportedError`
  synchronously; the options bag is type-checked BEFORE the algorithm
  lookup (primitive bags → TypeError "constructor parameter 2 is not of
  type 'Options'."); `toWellFormed` is ToBoolean-coerced.
- **Inheritance divergence (ledger #1):** instances are
  `instanceof WritableStream` and the instance prototype chain is wired in
  both, but only TypeScript's genuine `class extends` links the
  CONSTRUCTOR's prototype to the WritableStream function; the C++ jsg
  static chain does not. Subclassing via `class X extends
  crypto.DigestStream` works in both.
- `digest` and `bytesWritten` are brand-checked prototype accessors
  (`digest` moves to a per-instance own property in the unflagged legacy
  era); `Symbol.dispose` is brand-checked; `[object DigestStream]`
  branding under `set_tostring_tag`.

## Core semantics

- **Write settlement:** the hash update runs inside write() (chunks are
  consumed before the write settles — post-write mutation or detach cannot
  change the digest; metadata comes from internal slots). Chunks:
  ArrayBuffer, any view (offsets honored), strings; bare
  SharedArrayBuffers reject (views onto them are accepted); everything
  else rejects TypeError "DigestStream is a byte stream but received an
  object of non-ArrayBuffer/ArrayBufferView/string type on its writable
  side." Zero-length writes are no-ops.
- **String encoding:** WTF-8 per chunk by default (lone surrogate = ED A0
  80 — deliberately NOT TextEncoder's substitution; pinned for existing
  digests). `toWellFormed: true` substitutes U+FFFD with
  TextEncoderStream-style stateful pairing across chunks and a close-time
  flush of a dangling lead. `bytesWritten` counts UTF-8 bytes (bigint in
  every era).
- **Digest promise:** memoized identity (same object every access, before
  and after settling); resolves on close() with the digest ArrayBuffer;
  rejects on abort(reason) with the reason; never settled by abandonment
  or GC. close() is single-use (second close rejects, digest unaffected);
  writes after close reject without disturbing the digest.
- **`Symbol.dispose`:** errors the digest ("The DigestStream was
  disposed.") without touching stream state — the stream stays unlocked,
  zero-length writes still resolve, non-empty writes reject; idempotent;
  a no-op after close. **Rejection reporting diverges (ledger #2):** the
  TypeScript implementation marks the digest promise handled, so an
  abandoned rejected digest produces NO unhandledrejection report; C++
  reports it. A derived promise (`.then()` with no catch) still reports in
  both.
- **Pipes:** a valid pipeTo destination from user streams, transforms, and
  Response bodies (brand checks pass in both implementations).
- **Writer accounting:** desiredSize counts the in-flight chunk against the
  default HWM of 1 and recovers on settlement in BOTH implementations
  (contrast the compression suite's inert C++ desiredSize); ready stays
  settled. Write resolutions carry undefined (no thenable check); the
  digest ArrayBuffer gets exactly one thenable check per ledger #3. The
  constructor's options-bag getter may re-enter the API safely.

## Compatibility flags

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `workers_api_getters_setters_on_prototype` (2022-01-31) | `digest` as prototype accessor | `legacyDigestIsOwnInstanceProperty` |
| `set_tostring_tag` (2024-09-26) | `[object DigestStream]` branding | `legacyToStringTag` |
| `capture_async_api_throws` (2022-10-31) | pinned; the pre-flag invalid-chunk behavior (rejected promise ALSO reported as an uncaught exception even when handled) cannot be pinned in the harness — see legacy-shape.js | — |
| `streams_enable_constructors` + `transformstream_enable_standard_constructor` (2022-11-30) | pipe tests build standard sources | — |

`digest-cpp-pedantic.wd-test` runs the full module set with the dateless
opt-in `pedantic_wpt` added, pinning the ABSENCE of pedantic effects
(the implementation consults the flag nowhere; the reachable
standard-streams machinery changes nothing the suite pins).

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | Constructor static inheritance | `getPrototypeOf(crypto.DigestStream) !== WritableStream` | `=== WritableStream` | `isRealWritableStreamSubclass` |
| 2 | Abandoned digest rejection reporting | reported | marked handled, not reported | `abandonedDigestReporting` |
| 3 | Thenable-check stage for the digest ArrayBuffer (fires exactly once in both) | when the digest promise is awaited | inside close(), as the deferred resolves | `thenInterceptionDuringDigestResolution` |

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | WritableStream subclassing incl. ledger #1; branding; brand-checked accessors + dispose; subclassable |
| `construction.js` | case-insensitive algorithm matching (digest-compared); CRC exact spelling; unknown/missing algorithms throw synchronously; `{name}` object form; all 8 algorithms with sizes; option-bag Web IDL rules with exact TypeError; type-check-before-lookup ordering |
| `digest-vectors.js` | pinned md5/SHA-256/crc32 outputs for bytes, strings, non-Uint8 views, offset subarrays; AWS SDK checksum vectors; mixed-type write accumulation with bytesWritten; shared digestOf helper |
| `string-encoding.js` | WTF-8-not-TextEncoder default; toWellFormed matches TextEncoder for every lone-surrogate shape; inert for valid input and byte chunks; identical bytesWritten; every falsy spelling defaults off; ToBoolean coercion; stateful pair-joining across chunks; dangling-lead flush; per-chunk default encoding |
| `chunk-types.js` | non-byte chunk rejection with exact message; bare-SAB rejection + SAB-view acceptance; DataView offset honored |
| `digest-promise.js` | promise identity stability across settling; bytesWritten bigint semantics incl. zero-length and UTF-8 counting |
| `lifecycle.js` | close resolves; abort rejects (incl. after writes); write-after-close rejects without disturbing the digest; double close safe; abandoned stream/writer safe; unused stream safe |
| `dispose.js` | dispose errors digest + non-empty writes while stream state untouched; zero-length writes still resolve; idempotent; no-op after close |
| `unhandled-rejection.js` | ledger #2 reporting matrix + derived-promise reporting |
| `buffer-lifecycle.js` | consume-at-write: post-write mutation/detach invisible; lying metadata getters never consulted |
| `pipe-integration.js` | pipeTo from user streams; TransformStream chain; Response body |
| `large-payload.js` | 1MB+ chunk digesting |
| `gc-interplay.js` | GC never settles an abandoned digest; writer keeps a collected wrapper operable |
| `reentrancy.js` | staged thenable-check matrix (ledger #3); write issued from a write continuation preserves accumulation order; options-bag getter re-entering the constructor is safe and its value is honored |
| `backpressure.js` | desiredSize counts and recovers (parity); ready settled |
| `legacy-shape.js` | unflagged era: `[object Object]`; digest as own instance property (no prototype accessor); bytesWritten stays a bigint prototype accessor; flow unchanged |
| `which-impl.js` | implementation detection |
