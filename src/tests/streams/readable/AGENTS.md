# ReadableStream (default), ReadableStreamDefaultReader/Controller

Informal specification of the value-oriented ReadableStream as
implemented in workerd, derived from — and kept in lockstep with — this
suite. **The tests are the normative artifact.** Byte streams
(type:'bytes', BYOB) belong to the readable-byte suite; the pipeTo/
pipeThrough matrix belongs to piping/.

The suite COMPLEMENTS WPT (`//src/wpt:streams`). Probing the C++
expectedFailures showed many root-cause to a handful of construction
divergences (hwm Infinity rejection, validation order) rather than
behavioral gaps; the reentrancy family is mostly parity at finite hwm.

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | garbage underlyingSource | accepts null, rejects numbers | rejects null (spec), accepts numbers | `garbageSourceValidation` |
| 2 | ctor argument conversion order | source.type, hwm, size | strategy size, hwm, then source.type (spec) | `argumentConversionOrder` |
| 3 | invalid hwm (-1/NaN) + Infinity | TypeError; Infinity REJECTED (integer conversion) — root cause of many WPT reentrant/errors failures | RangeError; Infinity accepted (spec) | `highWaterMarkValidated` |
| 4 | pull counts (hwm 1, enqueue-in-pull) | 1,1,3 — first read served from queue without pull, deferred pulls batch | 1,2,3 (spec: pull after start and every read) | `pullCountShape` |
| 5 | pull on reading the LAST queued chunk | no pull | pulls (spec) | `pullOnLastChunkRead` |
| 6 | sync start() throw | captured; stream errored | escapes constructor (spec) | `syncStartThrow` |
| 7 | reader.closed after releaseLock | SAME promise object, settled state kept | REPLACED with TypeError 'This reader has been released' (spec) | `closedReplacedOnReleaseAfterClose`/`...Error` |
| 8 | size() errors stream then throws/returns Infinity | enqueue swallows | enqueue rethrows / RangeError (spec) | `sizeErrorsStreamThenThrows`/`...ReturnsInfinity` |
| 9 | invalid size() return (NaN/-1) | enqueue returns normally, stream errored (ds null); DEFECT: subsequent read() spins the isolate synchronously (unpinnable) | enqueue throws RangeError 'Invalid chunk size', reads reject same (spec) | `invalidSizeReturnValue` |
| 10 | queue total-size math | integer-ish: saturates (maxSafeInt shape -2,-1,1,0), fractional sizes truncate to 0; DEFECT: reading a fractional-size chunk spins the isolate | full double-precision per spec (all four WPT shapes) | `queue-math.js` (3 tests) |
| 11 | tee cancel composite | source cancel gets ONLY the pair-completing branch's reason; first branch's cancel promise fulfills immediately | AggregateError[r1,r2] (branch order, identity; intentional divergence from spec's array); LONE branch cancel promise PENDS until the other cancels (spec) — never await a lone branch cancel | `teeCancelReasonComposite`, `teeCancelReverseOrder` |
| 12 | from(string) | iterates per code unit ['h','i'] | single chunk ['hi'] (spec: throws — both diverge from spec) | `fromString` |
| 13 | async-iterator prototype | exposes constructor + next/return | next/return only | `iteratorPrototypeShape` |
| 14 | read() inside size() | in-flight chunk fed DIRECTLY to the reentrant read | chunk queued; the NEXT enqueue bypasses the queue into the reentrant read (spec) — deliveries swapped | `readInsideSize` |
| 15 | adopted body stream lock after consumption | released | kept locked | `bodyIdentityAndLockCoupling` |
| 16 | then-getter fires during a read cycle | 1 (harness context) | 2 | `thenGetterFireCountOnRead` |
| 17 | close-twice / enqueue-after-close / size-not-function / from-return validation messages | own texts | own texts | `closeTerminality`, `sizeMustBeFunction`, `fromReturnValidationMessages` |

Parity worth noting (probed, pinned): pull serialization (never
re-entered); pull/async-start rejection identity; error-undefined
preserved through closed; error() twice / after close are no-ops;
desiredSize lifecycle (1 → 0 close, null error, 0 cancel) and
enqueue-skips-queue-with-pending-read; cancel-with-pending-pull; cancel
reason identity + once; locked-stream cancel rejects without running the
hook; tee error propagation identity to both branches, tee pull-per-read
shape, tee after partial read; the tee-reentrancy crash regressions;
from() cancel plumbing identity through return(); async-iterator
protocol interleavings (return/next no-await); chunks held BY REFERENCE
(mutation visible, identity) + detach-while-queued observed; and the
ENTIRE integration surface except #15: body chunk normalization
(messages included: 'This ReadableStream did not return bytes.'),
disturbed-into-Response TypeError, locked-into-Response ACCEPTED,
clone-both-read, cancel-body-then-consume 'Body has already been used',
readAll small/big/failed paths, SELF fetch round-trips (fetch body +
Request body, the workerd#5113 shapes).

The ts cell additionally sets the internal-testing
`expose_draining_reader` flag, installing the
`ReadableStreamDrainingReader` global — the bulk-drain conduit the C++
bridge drives to consume TypeScript streams (conduit basics in the
identity suite's draining-reader.js). No such global exists under the
C++ implementation; `draining-reader.js` asserts both sides.

## Compatibility flags

| Flag | Pinned in main cells | Unflagged side |
| --- | --- | --- |
| `streams_enable_constructors` (2022-11-30) | yes (the suite's subject) | `readable-cpp-legacy` cell: ctor and .from() throw plain Errors naming the flag; ReadableStreamDefaultController not exposed; native-backed streams (Response bodies) fully usable incl. tee() |
| `transformstream_enable_standard_constructor` | yes (tee-reentrancy drives tee through TransformStreams) | transform suite's legacy cells |
| `capture_async_api_throws`, `workers_api_getters_setters_on_prototype`, `set_tostring_tag`, `fixup-transform-stream-backpressure`, `writable_stream_spec_compliant_writer` | yes | other suites' legacy cells |
| `pedantic_wpt` (dateless opt-in) | `readable-cpp-pedantic` cell | zero observable deltas on this suite's probed surface (cpp == pedantic in every probe) |

## Module map

| Module | Coverage |
| --- | --- |
| `api-surface.js` | globals, controller not constructable, getReader modes, locked lifecycle |
| `construction.js` | ledger #1-#3, default hwm 1 |
| `source-algorithms.js` | ledger #4-#6, pull serialization, rejection identity, cancel-with-pending-pull |
| `controller.js` | desiredSize accounting/terminal states, error idempotence, close terminality (#17), close-drains-queue |
| `reader.js` | read ordering, releaseLock, closed replacement (#7), undefined error, reader.cancel, reader swap |
| `cancel.js` | reason identity, locked-cancel, hook rejection identity, queue discard |
| `bad-strategies.js` | ledger #8, #9, size-not-function |
| `queue-math.js` | ledger #10 (WPT float shapes; cpp bounded observables only) |
| `tee.js` | migrated edge cases + error propagation + cancel composite (#11) + pull-per-read |
| `tee-reentrancy.js` | the three C++ push-loop crash regressions (from api/streams/streams-test.js) |
| `from.js` | 11 migrated + fromString (#12) + return validation messages |
| `async-iteration.js` | 7 migrated + no-await interleavings + proto shape (#13) |
| `reentrancy.js` | enqueue/close/cancel-in-size (parity) + read-in-size (#14; guard the size() or C++ captures every later chunk) |
| `buffer-lifecycle.js` | chunk by reference, detach observed |
| `integration-body.js` | readAll family, normalization (incl. detached views, resizable-extent pinning), clone, cancel-then-consume, SELF round-trips |
| `integration-locked-disturbed.js` | disturbed rejected, locked accepted, body identity + lock coupling (#15) |
| `gc.js` | pending read + async iteration survive gc() |
| `then-interceptors.js` | ledger #16 |
| `legacy-constructors.js` | the unflagged cell (see flags table) |
| `draining-reader.js` | TS only (C++ cell asserts the global's absence): a queued backlog plus the close sentinel swept in ONE batched read; value chunks pass through UNTOUCHED (object identity); pull-driven yields per read with EOF as a separate empty batch; expectedLength undefined; error/cancel propagation; lock exclusivity and release |
| `data-volumes.js` | value-stream volume axes: 4096-chunk counts, a 1 MiB single string chunk, 8 MiB aggregate (128 × 64 KiB), and 1 MiB through tee on both branches — every chunk index-encoded |

Consumed sources: streams-async-iterator-test.js (deleted),
streams-tee-edge-cases-test.js (value half), streams-test.js (from/
readAll/reader/cancel families; BYOB+writable+TransformStream tests
remain), api/streams/streams-test.js (tee-reentrancy trio +
ResponseTextLargeBody), ts-webstreams-test.js (three parity body tests;
its TS-identity assertions remain). streams-js-test.js is deliberately
untouched: its tests interleave value and byte sections, so its value
halves move when the readable-byte suite consumes the byte halves.

## IDL shape (deliberately not pinned here)

WebIDL function metadata — operation `.length` values (optional
arguments do not count), and promise-typed attributes/operations
REJECTING rather than throwing on a broken `this` — is enumerated
per-implementation by WPT's `idlharness.any.js`: the C++ implementation
carries the known deviations as expectedFailures in
`src/wpt/streams-test.ts`; the TypeScript implementation matches spec.
The suites do not duplicate that enumeration.
