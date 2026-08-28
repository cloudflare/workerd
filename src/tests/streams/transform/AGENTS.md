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
| 3 | cancel hook calls controller.error() | readable.cancel() fulfills; writable.close() rejects | BOTH reject with the hook's error (spec; the WPT cancel.any family) | `cancelHookErrorFanOut` |
| 4 | error() after terminate() with queued chunk | ignored: queue drains, then done | late error wins: reads reject (spec; WPT terminate.any) | `errorAfterTerminateWithQueuedChunk` |
| 5 | readableType/writableType validation | TypeError, trailing-period message | RangeError (spec; WPT general.any) | `readableWritableTypeValidation` |
| 6 | invalid highWaterMark (either strategy) | TypeError (jsg uint64 boundary) | RangeError "Invalid highWaterMark" | `highWaterMarkValidated` |
| 7 | then-getter fires settling a write→read cycle | 2 (harness context) | 3 | `thenGetterFireCount` |
| 8 | backpressure RELEASE at readable HWM | RACY: the pending write either completes or latches forever — the reason WPT backpressure.any is disabled for C++ ("A hanging Promise was canceled"); only the race-independent prefix is asserted | deterministic spec flow: drain releases the write | `backpressureAppliedAtReadableHwm` |

Parity worth noting (probed, pinned): cancel hook runs (not flush) with
the reason for both readable.cancel() and writable.abort(), exactly once
even for cancel-then-abort; terminate() reads done + rejects the writable
with the same TypeError message on both sides; hook invocation shape
(start/transform/flush get 1/2/1 args, receiver = transformer,
prototype-chain hooks work); the fixup backpressure LATCH engages on both
sides (only the release diverges, #8); default readable strategy is
hwm 0; chunk identity through the transform; a buffer detached while its
view is queued is observed detached by the reader; queue totals recorded
at write time.

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
| `construction.js` | ledger #5, #6 |
| `transformer-algorithms.js` | start/transform/flush ordering, async hooks, chunk-type freedom; hook shape + prototype-chain (parity) |
| `error-propagation.js` | sync/async start/transform/flush error fan-out across writes/close/readable (#1); controller.error() rejects reads |
| `backpressure.js` | writable desiredSize through the transform; dual strategies; default readable hwm 0; latch + racy release (#8) |
| `cancel-matrix.js` | cancel-hook reason/identity/once (parity) + fan-out (#3) |
| `terminate.js` | terminate fates (parity) + late error (#4) |
| `reentrancy.js` | size()-error UAF regressions (#2, no-crash guarantee) + sequential parity shape |
| `buffer-lifecycle.js` | chunk identity; detach-while-queued observed by reader (parity) |
| `then-interceptors.js` | ledger #7 |
| `roundtrip.js` | JS transform → ITS pipe does not hang (regression) |
| `gc.js` | write→read handoff survives gc() with the stream dropped (--expose-gc) |
| `legacy-identity-fallback.js` / `legacy-backpressure.js` | see Compatibility flags |
| `which-impl.js` / `helpers.js` | implementation detection; consume helpers |
