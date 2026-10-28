# TransformStream, TransformStreamDefaultController

An informal specification of the JS-backed TransformStream as implemented
in workerd, derived from — and kept in lockstep with — the test suite in
this directory. **The tests are the normative artifact.** Both the C++
implementation (`src/workerd/api/streams/transform.c++` over
`standard.c++`'s `TransformStreamDefaultController`) and the TypeScript
implementation (behind `typescript_implemented_streams`) are covered.

The suite COMPLEMENTS WPT (`//src/wpt:streams` runs transform-streams/
against both implementations). Probing the 30+ C++ expectedFailures
showed most narrow to a handful of root causes (below); several WPT
"failures" (properties.any's arg counts/prototype-chain, the cancel-hook
basics) are parity under direct observation and are pinned as such.

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | sync start() throw | captured; both sides errored, writes/close reject | escapes the constructor (spec) | `syncErrorDuringStart` |
| 2 | error() from readable-side size() with a PENDING read | size() runs during enqueue; the read rejects | direct handoff to the reader: size() never runs, the read gets the chunk | `sizeCallbackErrorDoesNotUAF`, `...AndThrowDoesNotUAF` (sequential shape is parity: `sizeCallbackErrorSequential`) |
| 3 | ASYNC cancel hook calls controller.error() (a sync hook is parity: the cancel fulfills, close rejects) | readable.cancel() fulfills; writable.close() rejects | BOTH reject with the hook's error (spec; the WPT cancel.any family) | `cancelHookErrorFanOut` |
| 4 | error() after terminate() with queued chunk | ignored: queue drains, then done | late error wins: reads reject (spec; WPT terminate.any) | `errorAfterTerminateWithQueuedChunk` |
| 5 | readableType/writableType validation | TypeError, trailing-period message | RangeError (spec; WPT general.any) | `readableWritableTypeValidation` |
| 6 | invalid highWaterMark (either strategy) | TypeError (jsg uint64 boundary) | RangeError "Invalid highWaterMark" | `highWaterMarkValidated` |
| 7 | then-getter fires settling a write→read cycle | 2 (harness context) | same | `thenGetterFireCount` |
| 8 | backpressure RELEASE at readable HWM | RACY: the pending write either completes or latches forever — the reason WPT backpressure.any is disabled for C++ ("A hanging Promise was canceled"); only the race-independent prefix is asserted | deterministic spec flow: drain releases the write | `backpressureAppliedAtReadableHwm` |
| 9 | readable strategy `highWaterMark: Infinity` | TypeError at construction (integer conversion) — the ROOT CAUSE of most WPT reentrant-strategies/errors.any C++ expectedFailures, whose scenarios never construct | accepted (spec) | `hwmInfinityRejected` |
| 10 | writer.close() inside size() (enqueue-triggered, hwm 1) | reentrant close wins: queued chunk dropped, first read done | chunk delivered, then done (spec) | `writerCloseInsideSize` |
| 11 | writable.abort() inside size() (enqueue-triggered, hwm 1) | reentrant abort wins: first read rejects the reason | chunk delivered, then reads reject (spec) | `writableAbortInsideSize` |
| 12 | read() inside size() at hwm 0 — total size() calls | 2 (size consulted again when the enqueued chunk is pulled) | 1 (spec) — the handoff/read-value shape is parity | `readInsideSize` |
| 13 | terminate() immediately after readable.cancel(), before start | cancel reason wins: closed/write reject the reason object | terminate wins: closed/write reject the terminate TypeError (spec) | `terminateAfterReadableCancel` |
| 14 | write parked at readable hwm 1, a read dequeues and a same-turn enqueue refills | no pull on a dequeue that leaves room (readable ledger #5): the write stays parked until a read finds the queue empty | the dequeue pulls and the write transforms, although the enqueue re-asserted backpressure first (spec: one wait) | `writeReleasedByDequeueSurvivesSameTurnEnqueue` |
| 15 | controller.error() in the turn whose read released a parked write | writable reference dropped and algorithms cleared before the write wakes: it takes the identity path into the errored readable and rejects with that enqueue's TypeError | rejects with the writable's stored error, the error() reason (spec) | `writeReleasedByReadThenErroredSameTurn` |
| 16 | readable.cancel() with a same-turn abort, terminate(), writable size() error or close on a started stream, with and without hooks | the cancel fulfills throughout; terminate() after it throws 'This ReadableStream is closed.'; closed keeps the cancel reason; abort-then-cancel without hooks rejects the cancel; cancel-then-close without hooks rejects the close; some closed/close outcomes differ under pedantic_wpt (not asserted) | spec (Node agrees): the cancel rejects with the writable's stored error when an abort, terminate() or size() error reached it before the cancel settled; abort-then-cancel and cancel-then-close fulfill | `cancelThenAbortSameTurn`, `abortThenCancelSameTurn`, `cancelThenTerminateAfterStart`, `cancelAfterWritableSizeError`, `cancelThenCloseSameTurn` |
| 17 | when transformer.start() starts the pair (a write queued during start) | transform() runs after the first marker, for a synchronous start() and a returned promise alike | spec (Node agrees): after the third marker chained on a promise fulfilled before construction (synchronous start()), after the fourth chained on a returned promise fulfilled in the same turn — the start promise is resolved with start()'s result and each inner controller settles a new promise with it | `startSettlementTiming` |
| 18 | transformer object after the stream closes (stream and controller still held) | retained | collectable (spec ClearAlgorithms; Node agrees); after abort, cancel, terminate() and error() both collect | `transformerCollectedAfterFinish` |
| 19 | write released by a read, reaching the sink after a same-turn readable.cancel() cleared the algorithms (writable still writable) | rejects with 'The readable side of this TransformStream is no longer readable.' (TypeError); with a cancel hook, transform() still runs after cancel() | spec undefined: transform() is not called and the write rejects with the writable's stored error (the cancel reason, or the cancel hook's rejection) once the cancel settles, with or without hooks; Node fulfills the write and drops the chunk | `writeReachesSinkAfterCancelClearedAlgorithms` |
| 20 | controller.enqueue() whose readable-side size() throws | enqueue() returns without throwing; with no re-entry in size() the write never settles; a readable.cancel() in size() errors the writable with the cancel reason | throws the readable's stored error (spec; Node agrees): e1 after error(e1), undefined after error(undefined), and undefined after readable.cancel() or terminate() (the closed readable has no stored error); the writable is errored with size()'s error unless an earlier error got there | `enqueueThrowsReadableStoredError` |
| 21 | (no divergence) `transformer.expectedLength` conversion, as the readable-byte suite's #35 describes: as any `uint64_t` converts, every failure a `TypeError` | same | same | `transformExpectedLengthRejected`, `transformExpectedLengthCoercion`, `transformExpectedLengthRange` (parity) |
| 22 | (no divergence) readable.cancel() then writer.write() in the same turn, with room in the readable: the write reaches the sink at once, after the cancel cleared the algorithms and before it settles. With no hooks, flush() only or transform(), the write rejects with the cancel reason and transform() is not called | same | same | `writeAfterCancelSameTurn` (parity) |
| 23 | a write whose readable-side size() calls readable.cancel() and then throws e2 (no hooks, flush() only, transform()) | size()'s throw is swallowed (#20): the write fulfills, and the writable is errored with the cancel reason | the spec (TransformStreamDefaultControllerEnqueue step 5), with or without hooks: the write rejects with the closed readable's stored error, undefined, and the writable is errored with e2 | `readableCancelInsideThrowingSize` |
| 24 | constructor argument conversion order, every transformer member | the transformer's members, in its own order (readableType, writableType, start, transform, flush, cancel, expectedLength), then each strategy: hwm and its ToNumber, then size (writable, then readable) | both strategies in that order, then the transformer's members in lexicographic order (spec) | `argumentConversionOrder` |
| 25 | null transformer (every other primitive throws TypeError, and a function is accepted with its members read, in both; spec) | accepted as no transformer (a WebIDL dictionary) | TypeError 'transformer must be an object' (spec: the argument is typed `object`) | `garbageTransformerValidation` |
| 26 | readable strategy without a highWaterMark (null or `{}`); either strategy otherwise converts as a WebIDL dictionary in both: a function's members are read, and every primitive except null throws TypeError (messages differ) | the ReadableStream default (1): the TransformStream default (0) applies only when the argument is omitted | 0 (spec ExtractHighWaterMark) | `strategyArgumentShapes` |
| 27 | which error wins when two are possible (a primitive transformer's TypeError beats a throwing strategy getter, and a throwing writable strategy getter beats readableType 'bytes', in both; every transformer member converts before readableType is checked, so a non-callable start or flush, TypeError with different messages, or a throwing start getter beats readableType 'bytes', and a non-callable start beats an invalid readable highWaterMark, in both; spec) | each highWaterMark is converted to an integer with the arguments: an invalid writable highWaterMark (TypeError) beats readableType 'bytes' and a throwing readable strategy getter | spec: both strategies are converted, then readableType is checked, then each highWaterMark's range, so readableType 'bytes' (RangeError) and a throwing readable strategy getter beat an invalid writable highWaterMark | `conversionErrorPrecedence` |
| 28 | the Content-Length of a fork (a tee branch, a branch teed again, the copy `new Request()` makes) of the readable side of a transformer declaring `expectedLength`, made before any read (made after a read, the forks go out chunked in both) | none: tee branches never carry a length, so they go out chunked | the declared length, as for a default source (the readable suite's ledger #35) | `transformForkContentLength` |

Parity worth noting (probed, pinned): cancel hook runs (not flush) with
the reason for both readable.cancel() and writable.abort(), exactly once
even for cancel-then-abort; terminate() reads done + rejects the writable
with the same TypeError message on both sides; hook invocation shape
(start/transform/flush get 1/2/1 args, receiver = transformer,
prototype-chain hooks work); the fixup backpressure LATCH engages on both
sides (only the release diverges, #8); an omitted readable strategy
is hwm 0 (one without a highWaterMark is #26); chunk identity through the transform; a buffer detached while its
view is queued is observed detached by the reader; queue totals recorded
at write time.

Reentrancy parity (probed at FINITE hwm, where the WPT originals use the
C++-rejected Infinity, #9): enqueue/terminate/readable.cancel/
writer.write/sync writer.write inside the readable-side size() all match
the WPT spec expectations on both sides — nested-enqueue chunk reversal
included — and controller.error() from size() preserves error-object
identity. controller.error() after a transformer hook throw is a no-op
on both sides (identity preserved). Only the ops in #10-#12 genuinely
diverge.

The ts cell additionally sets the internal-testing
`expose_draining_reader` flag, installing the
`ReadableStreamDrainingReader` global — the bulk-drain conduit the C++
bridge drives to consume TypeScript streams (conduit basics in the
identity suite's draining-reader.js). No such global exists under the
C++ implementation; `draining-reader.js` asserts both sides.

## Compatibility flags

| Flag (enable date) | Pinned in main cells | Unflagged side guarded by |
| --- | --- | --- |
| `transformstream_enable_standard_constructor` (2022-11-30) | yes (the suite's subject) | `transform-cpp-legacy` cell: ctor ignores its arguments (one-time warning) and returns the internal identity pipe — TransformStream-branded (jsg constructors wrap as the called class, NOT IdentityTransformStream), transformer hooks never run, byte pass-through, strings UTF-8-encoded |
| `fixup-transform-stream-backpressure` (2024-12-16; hyphenated flag name, unusually) | yes | `transform-cpp-legacy-backpressure` cell: the latch never engages, writes past the readable HWM complete immediately (deterministic — the flag-on race needs the latch) |
| `streams_enable_constructors`, `capture_async_api_throws`, `workers_api_getters_setters_on_prototype`, `set_tostring_tag`, `writable_stream_spec_compliant_writer` | yes | the writable/identity suites' legacy cells |
| `pedantic_wpt` (dateless opt-in) | `transform-cpp-pedantic` cell | its transform sites (finish-operation coordination) produce NO deltas on this suite's probed surface; the shared modules run unbranched |

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | transform globals; controller not constructable; bare ctor is standard pass-through, not ITS |
| `construction.js` | ledger #5, #6, #9, #24–#27; both strategies' members read once each (parity) |
| `transformer-algorithms.js` | ledger #17; start/transform/flush ordering, async hooks, chunk-type freedom; hook shape + prototype-chain (parity) |
| `error-propagation.js` | sync/async start/transform/flush error fan-out across writes/close/readable (#1); controller.error() rejects reads; error() no-op after hook throw (parity, identity); enqueue() once flush() has settled throws TypeError and leaves the writable's close untouched (parity) |
| `backpressure.js` | writable desiredSize through the transform; dual strategies; default readable hwm 0; latch + racy release (#8); a parked write released by a read transforms even when a same-turn enqueue re-asserts backpressure (parity at hwm 0; #14 at hwm 1) or rejects when the turn also errors the controller (#15) |
| `cancel-matrix.js` | cancel-hook reason/identity/once (parity) + fan-out, sync (parity) and async (#3) |
| `cancel-outcomes.js` | readable.cancel() outcome with a same-turn abort, terminate(), size() error or close, hooks and zero-hook path (#16); a write reaching the sink after a same-turn cancel cleared the algorithms, released by a read (#19) or written after the cancel (#22) |
| `terminate.js` | terminate fates (parity) + late error (#4) + terminate-after-cancel (#13) |
| `reentrancy.js` | size()-error UAF regressions (#2) + sequential/identity shapes; all 9 WPT reentrant-in-size() ops: 6 parity at finite hwm, #10-#12 divergences; what enqueue() throws when size() throws after re-entering (#20); how a write settles when its size() cancels the readable and throws (#23) |
| `buffer-lifecycle.js` | chunk identity; detach-while-queued observed by reader (parity) |
| `then-interceptors.js` | ledger #7 |
| `roundtrip.js` | JS transform → ITS pipe does not hang (regression); the workerd `TransformStream({ expectedLength })` extension surfaces a concrete Content-Length on fetch/Request bodies (parity; issue #5113 regression — the TS transform advertises the value through its readable's controller) |
| `gc.js` | write→read handoff survives gc() with the stream dropped (--expose-gc); the transformer is collectable once the stream is closed, aborted, cancelled, terminated or errored, with the stream held (#18 for close); the writable side, held alone, keeps the readable side alive while open and releases it once closed or aborted (parity) |
| `legacy-identity-fallback.js` / `legacy-backpressure.js` | see Compatibility flags |
| `draining-reader.js` | TS only (C++ cell asserts the global's absence): writes flow through the transformer into conduit reads; a readable-side backlog plus close sentinel swept in one batch; flush() output rides the final batch; expectedLength undefined; transformer errors propagate |
| `data-volumes.js` | volumes through JS transformers with concurrent producer/consumer: 1 MiB passthrough, 8 MiB XOR (proves every byte passed through the transformer), 4096-chunk value mapping |
| `pollution.js` | Object.prototype members stand in for neither an omitted transformer or strategy nor the internal source/sink dictionaries (neither implementation reads them) |
| `expected-length.js` | ledger #21, with the converted length observed as a fetch body's Content-Length (the `roundtrip.js` echo); integral lengths up to the uint64 maximum accepted; ledger #28, the Content-Length of forks of the readable side made before and after a read |
| `which-impl.js` / `helpers.js` | implementation detection; consume helpers |
