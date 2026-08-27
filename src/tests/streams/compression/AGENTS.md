# CompressionStream and DecompressionStream

An informal specification of the two Compression Streams classes as
implemented in workerd, derived from — and kept in lockstep with — the test
suite in this directory. **The tests are the normative artifact.** Both the
C++ implementation (`src/workerd/api/streams/compression.{h,c++}`, built on
internal streams over the shared `api/compression.h` CodecStage) and the
TypeScript implementation (`src/per_isolate/webstreams/compression.ts`,
behind `typescript_implemented_streams`) are covered. The two wrap the SAME
C++ zlib codec (the TS pair drives `utils.newCompressionCodec` handles), so
codec output is parity by construction; divergences live in the stream
wrappers.

The WPT `compression/*` tests already run against both implementations
(`//src/wpt:compression` and `//src/wpt:compression-ts`), so this suite
complements WPT: workerd-specific semantics (eager push, BYOB readable),
exact error identities and messages, the divergences, the strict-checks
flag axis, and real-HTTP body integration via the SELF loopback binding.

## Interfaces

```webidl
[Exposed=Worker]
interface CompressionStream {
  constructor(DOMString format);  // 'deflate' | 'gzip' | 'deflate-raw'
  readonly attribute ReadableStream readable;
  readonly attribute WritableStream writable;
};
[Exposed=Worker]
interface DecompressionStream {
  constructor(DOMString format);
  readonly attribute ReadableStream readable;
  readonly attribute WritableStream writable;
};
```

- **Inheritance divergence (ledger #5):** C++ subclasses `TransformStream`;
  TypeScript is standalone. `readable`/`writable` placement and
  `constructor.length` (0 vs 1) follow (#5, #6).
- The format is ToString-coerced exactly once (an object's `toString` runs
  once, both impls); a coerced string outside the three formats throws
  `TypeError` "The compression format must be either 'deflate',
  'deflate-raw' or 'gzip'." — but non-string arguments diverge (#7):
  TypeScript coerces `undefined` into that same validation failure, while
  the C++ jsg layer rejects it at the type boundary ("constructor parameter
  1 is not of type 'string'."). `null` coerces to `"null"` in both.
- The internal codec factory (`utils.newCompressionCodec`) is never a
  JS-visible surface.

## Core semantics

- **Eager push:** write() feeds the codec and settles when the codec has
  consumed the chunk — no read demand needed (not the identity rendezvous,
  not the encoding suite's HWM-0 demand-driven transform). Output buffers
  unboundedly on the readable side; a parked read is served as soon as any
  output exists (for deflate: the 2-byte zlib header from the first write,
  the rest at close-time flush).
- **Snapshot at write:** the codec consumes a copy taken synchronously
  inside write() (C++ adapter copy; TS strategy-size-callback snapshot).
  Post-write mutation/resize/detach cannot change what compresses; an
  already-detached chunk is a zero-byte no-op; shadowing metadata getters
  are never consulted.
- **Chunks:** ArrayBuffer, any view (offsets honored), empty and detached
  inputs are accepted by both. Strings (#1) and SharedArrayBuffers incl.
  SAB-backed views (#2) are accepted and encoded/copied by C++ but rejected
  by TypeScript per spec. Everything else rejects with TypeError (#3
  message) — after which the C++ stream SURVIVES (later writes flow, clean
  close) while TypeScript errors both sides (#4).
- **Corrupt input** (DecompressionStream): the WRITE rejects (TypeError
  "Decompression failed.") and both sides error, in both implementations;
  the failure propagates through downstream pipes to consumers.
- **strict_compression_checks** (pinned): trailing bytes after the member
  reject the write ("Trailing bytes after end of compressed data"); close()
  before the member completes rejects ("Called close() on a decompression
  stream with incomplete data") — including close with no data at all.
  Closing a CompressionStream with no writes emits a valid empty member.
- **Backpressure accounting (#8):** the C++ writer's `desiredSize` is inert
  (always 1); TypeScript counts the in-flight chunk against the default
  HWM 1 and recovers. `writer.ready` is settled in both (no sustained
  backpressure signal under eager settlement).
- **BYOB:** the readable is byte-capable in both (legacy parity; WHATWG
  describes a default stream).
- **Termination:** `writer.abort(reason)` errors both sides; a pending read
  receives the reason re-created across kj under C++ (same type/message
  with `enhanced_error_serialization` pinned, different instance) vs the
  original instance under TS (#9); `writer.closed` gets the ORIGINAL
  instance in both. Writes after abort: C++ closed-TypeError vs TS original
  reason (#10). Non-Error reasons are Error-ized by C++, delivered raw by
  TS (#11). The canceling reader's parked read: C++ rejects with the
  reason, TS resolves done (#12). `readable.cancel()` errors the writable
  under TS; C++ leaves it untouched — `writer.closed` stays pending (#13).
- **Reads:** a second concurrent default read rejects under C++ ("single
  pending read request") and parks under TS (#14); the thenable check runs
  once per read under C++, twice under TS (#15).
- **tee():** both branches observe identical bytes; the single-branch
  cancel promise carries the identity suite's ledger #13 semantics (C++
  immediate, TS shared composite).
- **Bodies:** the four SELF-loopback tests drive compression through real
  HTTP serialization (close-signal propagation from JS streams into
  internal response-body writables) plus response-body decompression and
  multi-transform chains.
- The #6061 unhandled-rejection regression (Array.fromAsync over a failing
  DecompressionStream) is pinned under
  `unhandled_rejection_after_microtask_checkpoint` in both cells.

## Compatibility flags

The C++ cell pins every date-gated flag the implementation is subject to
(see the comment in `compression-cpp.wd-test`); the TS cell pins only
`strict_compression_checks` (consulted by the shared codec factory),
`unhandled_rejection_after_microtask_checkpoint` (isolate-level event
timing), and the internal-testing `expose_draining_reader`.

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `strict_compression_checks` (2023-08-01) | DS trailing-data + incomplete-close errors | `legacy-nonstrict.js` |
| `capture_async_api_throws` (2022-10-31) | invalid chunk rejects instead of throwing synchronously | `legacyInvalidChunkThrowsSynchronously` |
| `unhandled_rejection_after_microtask_checkpoint` (2026-03-03) | rejection-event timing the #6061 regression depends on | — |
| internal-stream flags (BYOB, abort queue, getters, tags, error serialization; see config comment) | generic internal-stream behaviors | identity suite legacy cell |

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | String chunks | accepted, UTF-8 encoded (the WPT compression-bad-chunks expected failure) | rejected TypeError | `stringChunkDiverges` |
| 2 | SharedArrayBuffer / SAB-backed view chunks | accepted (copied out) | rejected TypeError | `sharedArrayBufferChunkDiverges` |
| 3 | Invalid-chunk TypeError message | "This TransformStream is being used as a byte stream, but received an object of non-ArrayBuffer/ArrayBufferView type on its writable side." | "The provided value is not of type (ArrayBuffer or ArrayBufferView)" | `invalidChunkAftermathDiverges` |
| 4 | Invalid-chunk aftermath | stream survives | both sides error | `invalidChunkAftermathDiverges` |
| 5 | `TransformStream` inheritance + accessor placement | subclass; readable/writable inherited | standalone; own accessors | `transformStreamInheritance` |
| 6 | `constructor.length` | 0 | 1 | `constructorSurface` |
| 7 | Missing/undefined format | jsg type-boundary TypeError ("not of type 'string'") | ToString-coerced into format validation | `nonStringFormatThrows` |
| 8 | `desiredSize` accounting | inert (always 1) | counts in-flight chunk, recovers | `desiredSizeAccounting` |
| 9 | Abort reason to pending read | re-created (same type/message, different instance) | original instance | `abortReasonIdentity` |
| 10 | Writes after abort | TypeError "This WritableStream has been closed." | original abort reason | `writeAfterAbortDiverges` |
| 11 | Non-Error abort reasons | Error-ized (string becomes message) | original value | `nonErrorAbortReasonSurfacing` |
| 12 | Canceling reader's parked read | rejects with the cancel reason | resolves done (WHATWG) | `cancelSettlesPendingRead` |
| 13 | `readable.cancel()` → writable side | untouched; `writer.closed` stays pending | errored; closed rejects with the reason | `cancelReadableWritableAftermath` |
| 14 | Second concurrent default read | TypeError "single pending read request" | parked, served in order | `secondConcurrentRead` |
| 15 | Thenable check per read resolution | once | twice | `thenInterceptionDuringReadResolution` |

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | toStringTag branding; codec factory not exposed; side stability; inheritance/placement (#5); ctor name/length/source (#6); `node:stream/web` re-exports are the same classes; accessor brand checks |
| `construction.js` | valid formats; invalid format exact message (case-sensitive); one-shot ToString coercion; non-string formats (#7) |
| `round-trip.js` | all-formats round trips (compression verified smaller); parked-read service with pinned deflate bytes; shared pump/concat/readAll helpers |
| `chunk-boundaries.js` | byte-at-a-time compression; 2-byte split decompression; all formats with 5-byte write chunks |
| `large-payload.js` | 400KB patterned payload, chunked writes, byte-exact round trip |
| `empty-stream.js` | close-with-no-writes emits a valid empty member; decompressing it yields EOF |
| `corrupt-input.js` | write-time rejection with "Decompression failed."; both-sides error; iteration rejection; bad magic bytes |
| `strict-checks.js` | trailing-data write rejection; close-with-no-data rejection; truncated-member close rejection |
| `chunk-types.js` | BufferSource acceptance incl. offsets; string (#1), SAB (#2), invalid-chunk message+aftermath (#3, #4) |
| `buffer-lifecycle.js` | snapshot-at-write: post-write mutation/detach/shrink invisible; already-detached no-op; lying metadata getters never consulted |
| `byob.js` | BYOB reader fills a 2-byte destination with the gzip magic |
| `backpressure.js` | eager write settlement without reads; desiredSize accounting (#8) |
| `propagation.js` | abort rejects pending read (reason per #9), errors both sides; cancel settles parked read (#12); write-after-abort (#10); non-Error reasons (#11); writes after a queued close reject (message per impl) without disturbing the close or output; cancel→writable aftermath (#13) |
| `reentrancy.js` | thenable-check counts (#15); second concurrent read (#14); close from a read continuation with round-trip integrity; sibling tee cancel from a continuation |
| `tee.js` | branches byte-identical; single-branch cancel (identity ledger #13 semantics) with survivor draining |
| `draining-reader.js` | TS only (C++ asserts absence): expectedLength undefined; a closed stream's buffered backlog swept in ONE read with done; lock/release |
| `gc-interplay.js` | writer abort after wrapper GC; decompression through collected wrapper (codec handle liveness) |
| `pipe-integration.js` | compress→decompress chains from user and TransformStream sources; through IdentityTransformStream; bad-data propagation through both transform kinds |
| `body-integration.js` | direct `Response(cs.readable)` body (same object, unlocked; byte-exact local arrayBuffer round trip); SELF-loopback HTTP: response-body decompression, multi-transform chain with completing pipeTo, compression/decompression pipelines into internal response bodies |
| `unhandled-rejection.js` | #6061: no spurious unhandledrejection with Array.fromAsync |
| `which-impl.js` | implementation detection |

## Legacy (unflagged) behaviors

Guarded by `compression-cpp-legacy.wd-test` (C++ only,
`generate_all_compat_flags_variant = False`):

| Behavior | Asserted by |
| --- | --- |
| Trailing bytes after the member tolerated; member content delivered | `legacyTrailingDataTolerated` |
| close() with no data tolerated (empty output) | `legacyCloseWithoutDataTolerated` |
| close() mid-member tolerated | `legacyTruncatedMemberCloseTolerated` |
| Invalid chunk throws synchronously; the stream survives | `legacyInvalidChunkThrowsSynchronously` |

Generic pre-flag internal-stream behaviors (BYOB in-place fills, property
placement, `[object Object]` stringification) are guarded by the identity
suite's legacy cell and not repeated here.
