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
- The queuing strategy is consulted for `highWaterMark` **only**. A
  user-supplied `size` member is never invoked and never affects
  accounting in either implementation (C++ reads `highWaterMark` alone;
  TypeScript installs its own internal size callback).

## Core semantics

### The rendezvous model

The identity stream is a byte pipe whose write promises settle on
**consumption, not enqueue**. Any number of writes — and a close — may be
queued without awaiting: the chunks buffer inside the stream, in order. But
each `write()` promise resolves only once reads have fully consumed that
write's bytes, `close()` only once everything queued before it has drained,
and a read completes only when there is something to deliver (data, EOF, or
an error). Nothing is delivered ahead of read demand; under TypeScript a
read's demand takes everything already written, including writes queued
behind the one the writable is on, so a write read ahead of its turn
settles when the writable reaches it, a few microtasks after its last byte
is read. (Contrast a standard
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
- `abort()` never waits for a read: a write parked for want of a reader
  rejects with the reason, as do the writes and close queued behind it,
  and the abort fulfills. What later reads reject with diverges (ledger
  #21).
- `readable.cancel(reason)` rejects a pending write and a pending close;
  subsequent writes reject. The canceling reader's own later reads resolve
  `done`, and its `closed` promise resolves; how a read still pending at
  the cancel settles diverges (ledger #20).

### BYOB reads

The readable side supports `getReader({ mode: 'byob' })` (unlike a standard
`TransformStream` readable):

- The destination view is filled at its real offset, bounded by its real
  extent; a write larger than the view is delivered across successive
  reads, and the write resolves only once fully consumed — its bytes stay
  counted in the writer's `desiredSize` until then.
- A read fills its view across write boundaries with everything already
  written (zero-length writes and invalid chunks in between deliver
  nothing; a close ends it); each write resolves once its last byte has
  been read. A read with a minimum (`readAtLeast`, `{ min }`) stays pending
  until the minimum is met, then takes everything already written, up to
  its view. The same holds for BYOB reads on tee branches. Under C++ a read
  carries at most one write's bytes, and a read with a minimum stops once
  it is met (ledger #22).
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
- `tee()` itself creates no demand; a single branch's read is demand.
  When the write then settles diverges (ledger #23). C++: once one branch
  has read it; the sibling's buffered copy is not counted against the
  writer's budget. TypeScript: backpressure follows the slowest branch — a
  write settles, and leaves the writer's `desiredSize`, once every branch
  has read its last byte, or once a branch is **starved** (it has a
  pending read and has taken everything delivered), as a pending read on
  any cursor overrides backpressure in the queue's pull rule. Either way,
  reading one branch (or one nested leaf) to completion never deadlocks:
  the unread branch buffers.
- Each branch's BYOB reads are sized independently and, under TypeScript,
  span the writes already made (ledger #22, #23); C++ caps a branch's reads
  at the sizes the first-reading branch requested.
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

`identity-cpp-pedantic.wd-test` runs the full shared module set with the
dateless opt-in `pedantic_wpt` flag added to the main C++ cell's pinned
set. Unlike the encoding suite's pedantic cell, it pins the ABSENCE of
pedantic effects: the internal-stream implementation consults the flag
nowhere and the pedantic-gated standard-streams machinery is unreachable
from this suite's surface, so every shared assertion holds unchanged — an
enforced invariant against future pedantic branches reaching these
classes.

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
| 6 | Invalid chunk aftermath | stream unaffected, remains usable | same — the sink signals the rejection through the non-fatal write-rejection channel; the write rejects, the stream survives (the per-write contract of the internal transforms) | `rejectsNumberChunk` |
| 7 | String `desiredSize` accounting | exact UTF-8 byte count | `length × 3` upper-bound estimate | `stringWriteDesiredSizeAccounting` |
| 8 | Abort/cancel reason identity | re-created `Error`, same message (crosses kj); exception: `writer.closed` under abort gets the original instance | original instance everywhere | `abort-propagation.js`, `cancel-propagation.js` |
| 9 | Writes after abort | `TypeError` "This WritableStream has been closed." | original abort reason | `abortRejectsSubsequentWrites` |
| 10 | Writes after cancel with close in flight | closed `TypeError` | original cancel reason | `cancelRejectsPendingWriteAndClose` |
| 11 | FLS enforcement | read-side `TypeError`; the offending write/close succeeds | eager write-side `RangeError`; readable errors too | `fixed-length-errors.js` |
| 12 | Already-detached `ArrayBuffer` chunk | zero-length no-op | rejects `TypeError` (per-write; the stream survives) | `alreadyDetachedBufferAtWrite` |
| 13 | Single tee-branch cancel promise | resolves immediately | WHATWG semantics: shared promise, settles when both branches cancel | `cancelOneBranchKeepsWriterFlowing` |
| 14 | Write after both tee branches cancel | parks forever (composite cancel not propagated to the writable) | rejects `AggregateError` "All readable stream tee branches were canceled" — also a write in flight when the last branch cancels, even if a branch had read it | `writeAfterBothBranchesCancel`, `inFlightWriteRejectsWhenBranchesLeaveAfterRead` |
| 15 | Piping between identity streams | not implemented: `pipeTo()` takes both locks then rejects `TypeError` ("Inter-TransformStream ReadableStream.pipeTo() is not implemented."); `pipeThrough()` throws it synchronously | fully functional: delivery, completion, and error propagation in both directions with original reason instances; circular `pipeThrough(its)` currently succeeds and locks both sides — `TODO(streams-ts)`: it should fail | `pipe-integration.js` |
| 16 | Second concurrent default read | rejected at `read()` time: `TypeError` "This ReadableStream only supports a single pending read request at a time." | parked and served in order | `closeFromReadContinuationWithSecondReadParked` |
| 17 | Default-HWM accounting | inert: `desiredSize` stays 1 regardless of buffered writes; `ready` never replaced | one unit per buffered chunk against the default HWM of 1: `desiredSize` goes negative, `ready` replaced until drained | `defaultHighWaterMarkAccounting` |
| 18 | `readAtLeast` validation | `TypeError` always: negative minimums visibly sign-extend to 18446744073709551615 before the element-count check rejects them; values ≥ 2^31 rejected at the jsg int boundary; in-range minimums exceeding the buffer rejected on the element count | `TypeError` for a negative minimum, `RangeError` for any minimum exceeding the view | `readAtLeastValidation` |
| 19 | Non-Error cancel reasons | always surfaces an Error: strings become the message, `undefined` becomes "Stream was cancelled.", standard error types preserved, custom subclass names preserved via the pinned `enhanced_error_serialization` | the original reason VALUE, untouched — same instance, same string, even `undefined` itself | `cancelReasonTypeSurfacing` |
| 20 | Read pending at the reader's own `cancel()` | rejects with the re-created cancel reason ("Stream was cancelled." for a bare cancel); the rejection is delivered through a promise adopted a tick later, which the unhandled-rejection tracker predating `unhandled_rejection_after_microtask_checkpoint` reports as unhandled before the read's handler runs (pinned where it surfaces, in `src/tests/node/http-server`'s legacy cell) | resolves `{ value: undefined, done: true }` (spec) | `cancelSettlesPendingRead` |
| 21 | Reads after an abort that cleared a parked write | reject `Error` "Network connection lost.": cancelling the parked sink write puts the transform into its disconnection error before the abort reason arrives | reject with the original abort reason, as after any abort | `abortParkedWriteErrorsReadable` |
| 22 | BYOB read with several writes already made | answered with at most one write's bytes (a read with a minimum stops once it is met); the next write waits for another read | fills its view with everything already written, across writes, up to a close — past a read's minimum too; each write settles once a read has its last byte | `byobReadSpansQueuedWrites`, `byobReadSpanningBoundaries`; readAtLeast over 1-byte writes in the r2-patterns suite (its ledger #5) |
| 23 | Write settlement and BYOB reads across tee branches | a write settles once one branch has read it (the sibling's copy uncounted); a branch's reads are capped at the sizes the first-reading branch requested | a write settles once the slowest branch has read it, or once a branch is starved (pending read, nothing left to take) — never once the writable is erroring, when it rejects instead; each branch's reads are sized independently and span writes | `teeCreatesNoDemand`, `singleBranchReadDrivesWriter`, `writerDesiredSizeAcrossTee`, `abortBeforeStarvedReadRejectsInFlightWrite`, `tee-byob.js` |

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | toStringTag branding; `FixedLengthStream` subclassing; `readable`/`writable` are `ReadableStream`/`WritableStream` instances, stable, enumerable prototype accessors (placement per ledger #5); constructor source text (native code under C++, not under TS); accessor brand checks |
| `construction.js` | valid lengths (0, 5, −0.0, `MAX_SAFE_INTEGER`, bigints, with strategy); coerced length observable via HWM cap; invalid lengths throw (types per ledger #1–3); inheritance (ledger #4); a user-supplied strategy `size` is never invoked (ITS and FLS, with and without explicit HWM) |
| `chunk-types.js` | accepted: `Uint8Array`, `ArrayBuffer`, `DataView` subrange, string→UTF-8, subarray offsets; rejected: numbers, plain objects (`TypeError`; per-write — the stream survives, ledger #6); an invalid chunk queued behind valid writes surfaces its error in FIFO order — earlier writes still deliver and later traffic still flows in both implementations |
| `zero-length-writes.js` | empty view / buffer / string are non-closing no-ops |
| `copy-semantics.js` | delivered chunk never aliases the source; source mutation after delivery is invisible; source is not detached |
| `buffer-lifecycle.js` | write-time snapshot survives later resize/detach in both implementations; degenerate write-time inputs (already-detached per ledger #12; detached and out-of-bounds typed-array and DataView views are no-ops); shadowing/throwing metadata getters never consulted |
| `ordering.js` | 1:1 write/read correspondence in both interleavings; multi-chunk aggregate integrity; clean EOF tails |
| `byob.js` | BYOB reader support; partial fills across reads with write completion on full consumption (still pending a macrotask after a partial read); reads spanning queued writes, past zero-length and invalid ones, stopping at a close, through FLS (#22); lying destination extents (at call and after enqueue) with sentinel overwrite guards; read-call validation (zero-length view, non-view, missing argument); input buffer detached by read with non-detachable (SAB-backed) destinations rejected; repeated EOF zero-length views with preserved buffers |
| `backpressure.js` | writes and close queue unboundedly with settlement on consumption (a write read ahead of its turn settles when the writable reaches it, so recovery is asserted after the writes settle); a partial BYOB read keeps the write pending and its bytes counted in `desiredSize`; advisory overfill (negative `desiredSize`); default HWM 1 with divergent accounting (ledger #17); explicit HWM as initial `desiredSize` (negative-zero HWM normalized to +0); byte-level tracking incl. in-flight bytes; string accounting (ledger #7); `ready` replacement and recovery |
| `close-propagation.js` | pending read resolves done; post-close reads done; buffered data drains before done; `closed` promises settle; writes after a queued close reject (message per impl) without disturbing the close or delivered bytes |
| `abort-propagation.js` | pending/subsequent reads and both `closed` promises reject (identity per ledger #8); modern abort clears a pending write, rejecting it with the abort reason (undefined or original instance); abort of a write parked in the sink settles without a read (ITS and FLS, after an earlier consumed write, through the stream with the writer released), rejecting the write, the writes and close queued behind it, and `writer.closed` with the original reason; reads afterwards (ledger #21); later writes (ledger #9) |
| `cancel-propagation.js` | pending write/close reject (ledger #8, #10); canceling reader's later reads resolve done, its `closed` resolves; a read pending at the cancel settles per ledger #20; in C++, cancellation of a pending `pipeTo()` sink write establishes the disconnection error before a later readable cancel reason |
| `fixed-length.js` | exact-length delivery (one and two chunks); `FLS(0)`; HWM capping incl. bigint; capped-HWM data flow |
| `fixed-length-errors.js` | over/underwrite and close-without-write error the stream with the documented messages (types/surfacing per ledger #11); abort skips the underwrite check |
| `tee.js` | both branches observe full content (ITS and FLS); single-branch read does not hang |
| `tee-backpressure.js` | tee creates no demand; write settlement across branches (ledger #23: one branch in C++; the slowest or a starved branch in TS, with `desiredSize` held until then); cancel semantics (ledger #13, #14) |
| `tee-byob.js` | BYOB reads of different sizes on each branch (ITS and FLS) with per-read write settlement; a lagging branch holding the writer's `desiredSize`; cancelling the slower branch settles writes; a BYOB branch spanning writes beside a default-reader branch; nested-tee leaves with different view sizes (ledger #22, #23) |
| `tee-nested.js` | nested tee delivers to all leaves in order; single-leaf read does not hang |
| `draining-reader.js` | TS only (C++ cell asserts the global's absence): `expectedLength` pass-through (bigint for FLS, undefined for ITS, undefined after release); a single read drains every synchronously buffered chunk plus the close sentinel in one batch (tee-sibling backlog), and over a rendezvous stream the first read sweeps every write made before it, one chunk per write (the close sentinel with them or in its own read); write/close settlement through the conduit; lock exclusivity and release |
| `body-integration.js` | Response/Request with identity-stream bodies: `text()` drives the rendezvous; `resp.body` is the same stream object (unwrapped, unconsumed, unlocked); FLS happy path and underwrite through body consumption (types per ledger #11); multi-megabyte patterned bodies verified byte-for-byte through `arrayBuffer()` (ITS and FLS Response, Request) |
| `pipe-integration.js` | `pipeTo`/`pipeThrough` between identity streams (ledger #15): TS delivery (small and multi-megabyte patterned bodies), completion, and both error-propagation directions with original reasons; C++ not-implemented wall (rejection for pipeTo, synchronous throw for pipeThrough); circular `pipeThrough(its)` with the `TODO(streams-ts)` pin |
| `payload-helpers.js` | shared machinery: continuous prime-modulus byte pattern for large-body generation and byte-exact verification |
| `reentrancy.js` | user code re-entering mid-processing: `Object.prototype.then` interception during read resolution (consulted once per read in C++, twice in TS) incl. a re-entrant `writer.close()` or `writer.abort()` from inside the getter (an abort rejects the write being answered, in TS); close/write/sibling-cancel from read continuations; second-concurrent-read divergence (ledger #16) |
| `lock-release.js` | `releaseLock()` rejects the released handle's `closed` promise with TypeError ("has been released"); the stream returns to a lockable state |
| `read-at-least.js` | `readAtLeast(min, view)` parks until min bytes accumulate across writes, EOF yields a zero-length view, unavailable on default readers; argument validation per ledger #18; a `read(view, { min })` spanning two writes settles the first and leaves the second pending until its last byte is read |
| `reader-writer-acquisition.js` | `WritableStreamDefaultWriter`/`ReadableStreamDefaultReader`/`ReadableStreamBYOBReader` are directly constructible (no streams_enable_constructors needed) and lock the stream; `getReader({mode})` validates and a failed acquisition leaves the stream unlocked |
| `cancel-reason-types.js` | the cancel-reason type matrix of ledger #19, asserted on both the pending write and the pending close |
| `gc-interplay.js` | a writer keeps its collected stream wrapper's underlying stream alive and operable (`--expose-gc`) |
| `pollution.js` | Object.prototype members (`type`, `autoAllocateChunkSize`, `expectedLength`, `start`, `size`, `highWaterMark`) reach neither the internal dictionaries nor an omitted strategy: ITS and FLS round-trip under pollution (neither implementation reads them) |
| `propagation-helpers.js`, `which-impl.js` | shared machinery: reason-identity policy, implementation detection |

## Legacy (unflagged) behaviors

Guarded by `identity-cpp-legacy.wd-test` (C++ only; the TypeScript
implementation does not implement the pre-flag behaviors):

| Behavior | Asserted by |
| --- | --- |
| BYOB fills happen in place: result aliases the caller's non-detached buffer, input view remains usable, fill stays inside the view's region | `legacyByobFillsInPlace` |
| A destination transferred while the read is parked is not written into: zero-length result, data lost | `legacyByobDestinationTransferredMidRead` |
| A destination shrunk while the read is parked truncates delivery to the completion-time size | `legacyByobDestinationShrunkMidRead` |
| A destination grown while the read is parked delivers normally with the tail untouched | `legacyByobDestinationGrownMidRead` |
| BYOB read at EOF resolves `value === undefined` | `legacyByobEofReturnsUndefined` |
| `abort()` waits for an in-flight write: both park until a read drains the write, then both settle | `legacyAbortWaitsForPendingWrite` |
| Abort with only a pending read behaves like the modern one | `legacyAbortWithPendingReadResolves` |
| Invalid chunk `write()` throws synchronously; the stream survives | `legacyInvalidChunkThrowsSynchronously` |
| `readable`/`writable` are enumerable own data properties; nothing on the prototype chain | `legacyPropertyPlacement` |
| Instances stringify as `[object Object]` | `legacyToStringTag` |
