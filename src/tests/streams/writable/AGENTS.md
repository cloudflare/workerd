# WritableStream, WritableStreamDefaultWriter, WritableStreamDefaultController

An informal specification of the JS-backed writable stream classes as
implemented in workerd, derived from — and kept in lockstep with — the test
suite in this directory. **The tests are the normative artifact.** Both the
C++ implementation (`src/workerd/api/streams/writable.{h,c++}` over
`standard.c++`'s `WritableImpl`/`WritableStreamJsController`) and the
TypeScript implementation (`src/per_isolate/webstreams/writable.ts`, behind
`typescript_implemented_streams`) are covered.

The suite COMPLEMENTS WPT (`//src/wpt:streams` runs writable-streams/
against both implementations): behaviors WPT already asserts identically on
both sides are not duplicated here. The WPT C++ expectedFailures for
writable-streams/ in `src/wpt/streams-test.ts` (aborting ×11,
bad-strategies ×2, bad-underlying-sinks ×2, constructor ×2, floating-point
×3, start ×1) seeded the ledger below; probing showed several "failures"
(the aborting.any family) come from testharness strictness while the
underlying behaviors are parity — those parity pins live in
`abort-matrix.js`.

## The startedness model (root of several divergences)

C++ invokes sink algorithms synchronously from `writer.write()`/`close()`
(without `pedantic_wpt`; with it they are deferred a microtask, which does
NOT change the outcomes below). TypeScript follows the spec's `[[started]]`
gating: no sink hook runs until the start promise has settled in a
microtask. A synchronous `write(); abort();` sequence therefore finds the
write already in flight under C++ (the sink runs; the write fulfills) but
still queued under TypeScript (the sink never runs; the write rejects with
the abort reason).

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | invalid highWaterMark at ctor | TypeError (jsg uint64 conversion messages) | RangeError "Invalid highWaterMark" (spec) | `highWaterMarkValidated` |
| 2 | sink.type validation | ignored (pedantic_wpt: RangeError) | RangeError "Invalid underlying sink type" (spec) | `sinkTypeValidation` |
| 3 | argument conversion order | sink dictionary first (sink.write, hwm, size) | strategy first (size, hwm ×2, sink.write; spec) | `argumentConversionOrder` |
| 4 | ready at construction (no backpressure) | pending after a microtask, fulfills on a later turn | fulfilled within a microtask (spec) | `readyFulfillTiming` |
| 5 | sync start() throw | captured; stream errored, writes reject | escapes the constructor (spec) | `newWritableStreamStartError` |
| 6 | abort() on an errored stream | rejects with the stored error | fulfills with undefined (spec) | `newWritableStreamAbortError` |
| 6b | concurrent aborts | a fresh promise per abort() call | the same promise for both (spec); both fulfill undefined | `concurrentAbortPromiseIdentity` |
| 7 | startedness (see above) | sync write→abort: sink runs, write fulfills; a same-turn mutation/detach/resize of the chunk's buffer after write() is NOT observed by the sink | write still queued: rejected with abort reason; the buffer change IS observed | `writableStreamAbortWhileWriting`, `writableStreamAbortWriteClosePending`, `writableStreamPromisesResolvedInOrder`, `writableStreamCloseThrowRejectsPromises`, buffer-lifecycle.js (`chunkMutationVisibility`, `detachAfterWriteTiming`, `resizableGrowAfterWrite`, `resizableShrinkOutOfBounds`) |
| 8 | close hook racing an immediate abort | close hook runs; close+abort reject with its error | close still queued: hook never runs, close rejects with abort reason, abort fulfills | `writableStreamCloseThrowRejectsPromises` |
| 9 | queue totals | size() → uint64 (fractions truncate; NaN/negative/±Infinity → TypeError); desiredSize narrowed through `int` (wraps past 2^31) | double arithmetic per spec; invalid size → RangeError "Invalid chunk size" | `floatingPointQueueTotals`, `fractionalSizeTruncation`, `invalidSizeReturnRejects` |
| 10 | signal.reason for reasonless abort() | undefined (pedantic_wpt: AbortError DOMException) | AbortError DOMException (spec) | `abortSignalReason` |
| 11 | desiredSize while erroring | queue accounting value (pedantic_wpt: null) | null (spec) | `desiredSizeWhileErroring` |
| 12 | non-callable size / released-writer messages | jsg dictionary / "This WritableStream writer has been released." | TS validator / "This writer has been released" | `nonCallableSizeThrows`, `releaseLockInsideSize` |

Parity worth noting (probed, pinned): the whole in-flight abort matrix —
abort-before-start reason identity on ready/closed, errored-state reason
identity, sink abort suppressed after a bad-strategy error or a
pre-existing controller error, in-flight write finishing with rejection
during abort, an abort during a slow in-flight write leaving the write to
FULFILL, both orders of `abort()`×`controller.error()` during an
in-flight write, sink abort waiting for in-flight start/write/close
(`abort-matrix.js`, `writableStreamAbortTiming`,
`errorRaceWithCloseWritable`); sink hook getters read exactly once at
construction; a later sink write returning a rejected promise rejects
that write, a REPLACED ready, and closed with the sink's error; sink
write/close never run after a start() throw, however the failure
surfaced; ready is replaced as soon as desiredSize reaches 0; a detached
ArrayBuffer is an acceptable chunk (byteLength 0); a coercing
`{valueOf}` highWaterMark is accepted via ToNumber; the user size() runs
with an undefined receiver and one argument; reentrant `writer.write()`
from size() enqueues the inner chunk first; size() is never consulted
for doomed writes (the TS sinks' `willAcceptWrite` invariant); a patched
`Object.prototype.then` getter never fires while settling writer
promises (they resolve with undefined).

## Compatibility flags

| Flag (enable date) | Pinned in main cells | Unflagged side guarded by |
| --- | --- | --- |
| `streams_enable_constructors` (2022-11-30) | yes (the suite's subject) | `writable-cpp-legacy` cell: ctor throws a flag-pointing Error; `WritableStreamDefaultController` global absent |
| `capture_async_api_throws` (2022-10-31) | yes | `writable-cpp-legacy-writer` cell: double close throws synchronously ("Cannot close a writer that is already being closed") |
| `writable_stream_spec_compliant_writer` (2026-03-24) | yes | `writable-cpp-legacy-writer` cell: releaseLock() inside size() does not doom the write; release leaves a resolved ready in place |
| `workers_api_getters_setters_on_prototype`, `set_tostring_tag` | yes | generic placement/branding; identity suite legacy cell |
| `internal_writable_stream_abort_clears_queue` (2024-09-02) | NOT pinned | internal-impl only (`internal.c++`); unreachable from JS-backed WritableStream |
| `pedantic_wpt` (dateless opt-in) | `writable-cpp-pedantic` cell | flag-off sides are the C++ columns of ledger #2/#10/#11 |

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | writable globals exist; controller not constructable; bare ctor works (full IDL shape is WPT's) |
| `construction.js` | ledger #1–#5, #12; fractional and ToNumber-coerced hwm accepted |
| `sink-algorithms.js` | which sink hooks run with what arguments/controller; sync+async hook errors surface on writer promises (#5, #6); size() consulted per write; hook getters read once; second-write rejection fan-out; hooks silent after start throw |
| `write-semantics.js` | chunk identity (subarrays, any JS value via Object.is); multiple pending writes; settlement ordering incl. under abort (#7) |
| `buffer-lifecycle.js` | chunks never copied/validated: already-detached AB accepted (byteLength 0); post-write() mutation, detach, resizable grow, and shrink-out-of-bounds all observed per the startedness model (#7); size() runs inside write() so queue totals are immune to later detach |
| `close-semantics.js` | close-throw promise fan-out vs abort (#7, #8); double close rejects TypeError |
| `abort-semantics.js` | migrated abort lifecycle: reason propagation, signal event, persistent errored state, in-flight sequencing, terminal-state interactions (#7) |
| `abort-matrix.js` | probed parity matrix (see above) + signal reason (#10) + concurrent-abort identity (#6b) |
| `backpressure.js` | desiredSize accounting/recovery; ready replaced at capacity; WPT floating-point scenarios (#9); erroring desiredSize (#11) |
| `reentrancy.js` | size()-reentrant write ordering; releaseLock inside size (#12; flag-gated, cf. legacy-writer); controller.error inside write hook; doomed-write size skip; size receiver/arity |
| `then-interceptors.js` | then-getter never fires on writer promise settlement |
| `gc.js` | pending write survives gc() with all user refs dropped (--expose-gc) |
| `legacy-ctor-gate.js` | fully-unflagged: ctor Error + absent controller global |
| `legacy-writer.js` | pre-flag writer semantics (see Compatibility flags) |
| `data-volumes.js` | write-side volumes: 4096 × 16 B writes, single 1 MiB write, 1 MiB / 8 MiB chunked with writer.ready honored; the sink verifies the continuous prime-modulus pattern as chunks arrive |
| `which-impl.js` | implementation + pedantic detection |

## IDL shape (deliberately not pinned here)

WebIDL function metadata — operation `.length` values (optional
arguments do not count), and promise-typed attributes/operations
REJECTING rather than throwing on a broken `this` — is enumerated
per-implementation by WPT's `idlharness.any.js`: the C++ implementation
carries the known deviations as expectedFailures in
`src/wpt/streams-test.ts`; the TypeScript implementation matches spec.
The suites do not duplicate that enumeration.
