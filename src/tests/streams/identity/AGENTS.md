# IdentityTransformStream and FixedLengthStream

An informal specification of the two workerd-specific identity stream
surfaces, derived from — and kept in lockstep with — the test suite in this
directory. **The tests are the normative artifact**; this document is the
index that maps every specified behavior to the test that asserts it. Both
the legacy C++ implementation (`src/workerd/api/streams/`) and the
TypeScript implementation (`src/per_isolate/webstreams/identity.ts`, behind
`typescript_implemented_streams`) are covered; where they deliberately
diverge, both sides are specified and pinned.

Unless marked otherwise, behaviors below describe the **current default**
semantics — the six compatibility flags pinned by `identity-cpp.wd-test`
all enabled. Pre-flag behaviors are specified in the
[Legacy behaviors](#legacy-unflagged-behaviors) section and guarded by
`identity-cpp-legacy.wd-test`.

## Interfaces

Informal WebIDL. Coercion and error-type details deliberately live in the
assertion catalogue rather than in IDL annotations, because the two
implementations diverge on them.

```webidl
dictionary IdentityTransformStreamQueuingStrategy {
  (double or bigint) highWaterMark;
};

[Exposed=Worker]
interface IdentityTransformStream {
  constructor(optional IdentityTransformStreamQueuingStrategy queuingStrategy = {});
  readonly attribute ReadableStream readable;
  readonly attribute WritableStream writable;
};

[Exposed=Worker]
interface FixedLengthStream : IdentityTransformStream {
  constructor((double or bigint) expectedLength,
              optional IdentityTransformStreamQueuingStrategy queuingStrategy = {});
};
```

Interface notes:

- **Inheritance divergence:** in C++, `IdentityTransformStream` is itself a
  subclass of `TransformStream`; in TypeScript it deliberately is not
  (matching the `CompressionStream` / `TextEncoderStream` convention).
  `FixedLengthStream : IdentityTransformStream` holds in both.
- `readable` and `writable` are enumerable get-accessors on a prototype
  (under `workers_api_getters_setters_on_prototype`), stable across repeated
  access, and brand-checked. *Which* prototype holds them diverges: C++
  inherits them from `TransformStream.prototype`; TypeScript defines them on
  `IdentityTransformStream.prototype`.
- Instances are branded via `Symbol.toStringTag` (under `set_tostring_tag`):
  `[object IdentityTransformStream]` / `[object FixedLengthStream]`.

## Core semantics

### The rendezvous model

The identity stream is a byte pipe whose write promises settle on
**consumption, not enqueue**. Any number of writes — and a close — may be
queued without awaiting: the chunks buffer inside the stream, in order. But
each `write()` promise resolves only once reads have fully consumed that
write's bytes, `close()` only once everything queued before it has drained,
and a read completes only when there is something to deliver (data, EOF, or
an error). Nothing is delivered ahead of read demand. (Contrast a standard
`TransformStream`, where `write()` settles once the chunk passes into the
readable's queue, before any read occurs.)

- Chunks map 1:1 to default reads, in order, under either interleaving
  (writes queued first, or each read parked before its write).
- Aggregate content is preserved exactly across chunk boundaries.
- Writer-side backpressure counts **bytes** (not chunks), including bytes
  still in flight, and recovers as reads consume them. Default
  `highWaterMark` is 1; an explicit `highWaterMark` becomes the writer's
  initial `desiredSize`. The budget is **advisory**, not enforced: writes
  beyond it are still accepted and buffered, with `desiredSize` going
  negative to report the deficit. `writer.ready` is replaced while
  backpressure is on and settles as consumption frees budget.

### Chunk handling

- Accepted chunk types: `ArrayBufferView` (any, honoring offset/length),
  `ArrayBuffer`, and strings, which are UTF-8 encoded. Anything else fails
  with `TypeError`.
- Zero-length chunks (empty view, empty buffer, empty string) are no-ops:
  they resolve without delivering a chunk and without closing the stream.
- Writes **copy** their bytes; the delivered chunk never aliases the
  caller's buffer, and the caller's buffer is never detached by a write.
  The copy is taken synchronously inside `write()` in both
  implementations, so resizing or detaching the buffer after `write()`
  returns cannot change — or destroy — what gets delivered.
- Buffer metadata must be read from internal slots only — shadowing own
  properties (`byteLength`, `byteOffset`, `buffer`, `constructor`) are never
  consulted, whether installed before or after the write.

### FixedLengthStream length enforcement

`FixedLengthStream(expectedLength)` promises exactly `expectedLength` bytes:

- Delivering exactly `expectedLength` bytes then closing succeeds;
  `FixedLengthStream(0)` closes cleanly with no bytes.
- Writing more errors the stream ("too many bytes"); closing before all
  bytes were delivered errors the stream ("did not see all expected
  bytes"). Error type and surfacing point diverge (see ledger).
- `abort()` is not `close()`: aborting with undelivered bytes is not an
  underwrite error.
- The effective `highWaterMark` is capped at `expectedLength` (bigint
  lengths included); a smaller explicit `highWaterMark` is kept.
- The coerced `expectedLength` is observable through that cap:
  `min(expectedLength, highWaterMark)` is the writer's initial
  `desiredSize`.

### Close, abort, and cancel propagation

- `writer.close()` resolves a pending read as `done`, drains buffered data
  first, and settles both `closed` promises.
- `writer.abort(reason)` rejects pending and subsequent reads and both
  `closed` promises. Reason identity across the boundary diverges (ledger).
- `readable.cancel(reason)` rejects a pending write and a pending close;
  subsequent writes reject. The canceling reader's own reads resolve
  `done`.

### BYOB reads

The readable side supports `getReader({ mode: 'byob' })` (unlike a standard
`TransformStream` readable):

- The destination view is filled at its real offset, bounded by its real
  extent; a write larger than the view is delivered across successive
  reads, and the write resolves only once fully consumed.
- Under `streams_byob_reader_detaches_buffer`, the input buffer is
  transferred; the result is a view over the transferred buffer. At EOF
  (under `internal_stream_byob_return_view`) a BYOB read resolves `done`
  with a zero-length view whose buffer preserves its `byteLength` for
  reuse.
- The view's own properties are never consulted — neither at `read(view)`
  time nor later when a parked request is fulfilled; the extent is captured
  internally at enqueue.

### Bodies and piping (typical usage)

- `new Response(its.readable)` / `new Request(url, { method, body:
  its.readable })` neither wrap, consume, nor lock the stream: `resp.body`
  is the very same `ReadableStream` object, and `text()` (or a body reader)
  drives the rendezvous exactly like a direct reader. FixedLengthStream
  enforcement surfaces through body consumption with the ledger #11
  error-type divergence.
- Piping between identity streams diverges wholesale (ledger #15): the C++
  implementation does not implement inter-transform pumping — `pipeTo()`
  rejects and `pipeThrough()` throws "Inter-TransformStream
  ReadableStream.pipeTo() is not implemented" — while TypeScript implements
  the full pipe with error propagation in both directions.

### tee()

- Both branches observe the full content, including through nested tees
  (`branch.tee()`), with ordering preserved to every leaf.
- `tee()` itself creates no demand; a **single** branch's read is
  sufficient to drive the writer (write resolution and `desiredSize`
  recovery), while the other branch buffers a copy that is not counted
  against the writer's budget. Reading one branch (or one nested leaf) to
  completion never deadlocks.
- Canceling one branch leaves the writer flowing to the survivor. The
  cancel promise semantics and the fate of writes after **both** branches
  cancel diverge (ledger).

## Compatibility flags

The main cells pin these six semantic flags; the legacy cell omits them all.
`identity-ts.wd-test` omits the ones the TypeScript implementation does not
consult — its variants prove TS behavior is identical with them off or on.

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `streams_byob_reader_detaches_buffer` (2021-11-10) | BYOB input buffer is transferred | `legacyByobFillsInPlace` |
| `workers_api_getters_setters_on_prototype` (2022-01-31) | prototype accessors | `legacyPropertyPlacement` |
| `capture_async_api_throws` (2022-10-31) | invalid chunk → rejected promise | `legacyInvalidChunkThrowsSynchronously` |
| `internal_stream_byob_return_view` (2024-05-13) | BYOB EOF → zero-length view | `legacyByobEofReturnsUndefined` |
| `internal_writable_stream_abort_clears_queue` (2024-09-02) | abort clears in-flight write | `legacyAbortWaitsForPendingWrite` |
| `set_tostring_tag` (2024-09-26) | `Symbol.toStringTag` branding | `legacyToStringTag` |

`identity-ts.wd-test` additionally sets the internal-testing
`expose_draining_reader` flag, which the TS bootstrap consults to install
the `ReadableStreamDrainingReader` global — the bulk-drain conduit the C++
bridge drives to consume TypeScript streams and to read a
`FixedLengthStream`'s `expectedLength` for `Content-Length` derivation. No
such global exists under the C++ implementation; `draining-reader.js`
asserts both sides.

## Divergence ledger (C++ vs TypeScript)

Every entry is asserted on both sides via the `which-impl` conditional
pattern; a change to either side fails its cell.

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | FLS invalid-length error type | `TypeError` for every invalid length | `RangeError` (range/non-integer), `TypeError` (wrong type) | `fixedLengthInvalidLengths` |
| 2 | FLS length coercion | truncates non-integers (`0.5`→0, `2.9`→2), converts numeric strings (`'10'`→10) | rejects both | `fixedLengthCoercionDivergence` |
| 3 | FLS length range | rejects > 2^53−1 (`TypeError`) | accepts full uint64 | `fixedLengthLengthsAboveMaxSafeInteger` |
| 4 | `TransformStream` inheritance | `its instanceof TransformStream` is true | false (deliberate) | `identityBrandChecks` |
| 5 | Accessor placement | inherited from `TransformStream.prototype` | own on `IdentityTransformStream.prototype` | `propertyPlacement` |
| 6 | Invalid chunk aftermath | stream unaffected, remains usable | stream errors; `closed` rejects, later writes reject | `rejectsNumberChunk` |
| 7 | String `desiredSize` accounting | exact UTF-8 byte count | `length × 3` upper-bound estimate | `stringWriteDesiredSizeAccounting` |
| 8 | Abort/cancel reason identity | re-created `Error`, same message (crosses kj); exception: `writer.closed` under abort gets the original instance | original instance everywhere | `abort-propagation.js`, `cancel-propagation.js` |
| 9 | Writes after abort | `TypeError` "This WritableStream has been closed." | original abort reason | `abortRejectsSubsequentWrites` |
| 10 | Writes after cancel with close in flight | closed `TypeError` | original cancel reason | `cancelRejectsPendingWriteAndClose` |
| 11 | FLS enforcement | read-side `TypeError`; the offending write/close succeeds | eager write-side `RangeError`; readable errors too | `fixed-length-errors.js` |
| 12 | Already-detached `ArrayBuffer` chunk | zero-length no-op | rejects `TypeError`, errors the stream | `alreadyDetachedBufferAtWrite` |
| 13 | Single tee-branch cancel promise | resolves immediately | WHATWG semantics: shared promise, settles when both branches cancel | `cancelOneBranchKeepsWriterFlowing` |
| 14 | Write after both tee branches cancel | parks forever (composite cancel not propagated to the writable) | rejects `AggregateError` "All readable stream tee branches were canceled" | `writeAfterBothBranchesCancel` |
| 15 | Piping between identity streams | not implemented: `pipeTo()` takes both locks then rejects `TypeError` ("Inter-TransformStream ReadableStream.pipeTo() is not implemented."); `pipeThrough()` throws it synchronously | fully functional: delivery, completion, and error propagation in both directions with original reason instances; circular `pipeThrough(its)` currently succeeds and locks both sides — `TODO(streams-ts)`: it should fail | `pipe-integration.js` |

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | toStringTag branding; `FixedLengthStream` subclassing; `readable`/`writable` are `ReadableStream`/`WritableStream` instances, stable, enumerable prototype accessors (placement per ledger #5); accessor brand checks |
| `construction.js` | valid lengths (0, 5, −0.0, `MAX_SAFE_INTEGER`, bigints, with strategy); coerced length observable via HWM cap; invalid lengths throw (types per ledger #1–3); inheritance (ledger #4) |
| `chunk-types.js` | accepted: `Uint8Array`, `ArrayBuffer`, `DataView` subrange, string→UTF-8, subarray offsets; rejected: numbers, plain objects (`TypeError`; aftermath per ledger #6) |
| `zero-length-writes.js` | empty view / buffer / string are non-closing no-ops |
| `copy-semantics.js` | delivered chunk never aliases the source; source mutation after delivery is invisible; source is not detached |
| `buffer-lifecycle.js` | write-time snapshot survives later resize/detach in both implementations; degenerate write-time inputs (already-detached per ledger #12, out-of-bounds views); shadowing/throwing metadata getters never consulted |
| `ordering.js` | 1:1 write/read correspondence in both interleavings; multi-chunk aggregate integrity; clean EOF tails |
| `byob.js` | BYOB reader support; partial fills across reads with write completion on full consumption; lying destination extents (at call and after enqueue) with sentinel overwrite guards; EOF zero-length view with preserved buffer |
| `backpressure.js` | writes and close queue unboundedly with settlement on consumption; advisory overfill (negative `desiredSize`); default HWM 1; explicit HWM as initial `desiredSize` (negative-zero HWM normalized to +0); byte-level tracking incl. in-flight bytes; string accounting (ledger #7); `ready` replacement and recovery |
| `close-propagation.js` | pending read resolves done; post-close reads done; buffered data drains before done; `closed` promises settle |
| `abort-propagation.js` | pending/subsequent reads and both `closed` promises reject (identity per ledger #8); later writes (ledger #9) |
| `cancel-propagation.js` | pending write/close reject (ledger #8, #10); canceling reader's reads resolve done |
| `fixed-length.js` | exact-length delivery (one and two chunks); `FLS(0)`; HWM capping incl. bigint; capped-HWM data flow |
| `fixed-length-errors.js` | over/underwrite and close-without-write error the stream with the documented messages (types/surfacing per ledger #11); abort skips the underwrite check |
| `tee.js` | both branches observe full content (ITS and FLS); single-branch read does not hang |
| `tee-backpressure.js` | tee creates no demand; one branch drives the writer; sibling copy uncounted; cancel semantics (ledger #13, #14) |
| `tee-nested.js` | nested tee delivers to all leaves in order; single-leaf read does not hang |
| `draining-reader.js` | TS only (C++ cell asserts the global's absence): `expectedLength` pass-through (bigint for FLS, undefined for ITS, undefined after release); a single read drains every synchronously buffered chunk plus the close sentinel in one batch (tee-sibling backlog), while a rendezvous stream yields one chunk per read via the always-makes-progress fallback; write/close settlement through the conduit; lock exclusivity and release |
| `body-integration.js` | Response/Request with identity-stream bodies: `text()` drives the rendezvous; `resp.body` is the same stream object (unwrapped, unconsumed, unlocked); FLS happy path and underwrite through body consumption (types per ledger #11); multi-megabyte patterned bodies verified byte-for-byte through `arrayBuffer()` (ITS and FLS Response, Request) |
| `pipe-integration.js` | `pipeTo`/`pipeThrough` between identity streams (ledger #15): TS delivery (small and multi-megabyte patterned bodies), completion, and both error-propagation directions with original reasons; C++ not-implemented wall (rejection for pipeTo, synchronous throw for pipeThrough); circular `pipeThrough(its)` with the `TODO(streams-ts)` pin |
| `payload-helpers.js` | shared machinery: continuous prime-modulus byte pattern for large-body generation and byte-exact verification |
| `propagation-helpers.js`, `which-impl.js` | shared machinery: reason-identity policy, implementation detection |

## Legacy (unflagged) behaviors

Guarded by `identity-cpp-legacy.wd-test` (C++ only; the TypeScript
implementation does not implement the pre-flag behaviors):

| Behavior | Asserted by |
| --- | --- |
| BYOB fills happen in place: result aliases the caller's non-detached buffer, input view remains usable, fill stays inside the view's region | `legacyByobFillsInPlace` |
| BYOB read at EOF resolves `value === undefined` | `legacyByobEofReturnsUndefined` |
| `abort()` waits for an in-flight write: both park until a read drains the write, then both settle | `legacyAbortWaitsForPendingWrite` |
| Abort with only a pending read behaves like the modern one | `legacyAbortWithPendingReadResolves` |
| Invalid chunk `write()` throws synchronously; the stream survives | `legacyInvalidChunkThrowsSynchronously` |
| `readable`/`writable` are enumerable own data properties; nothing on the prototype chain | `legacyPropertyPlacement` |
| Instances stringify as `[object Object]` | `legacyToStringTag` |
