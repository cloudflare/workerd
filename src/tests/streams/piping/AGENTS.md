# pipeTo / pipeThrough

Informal specification of stream piping as implemented in workerd,
derived from — and kept in lockstep with — this suite. **The tests are
the normative artifact.** Endpoint behaviors belong to the sibling
suites (writable/, transform/, readable/, readable-byte/); identity↔
identity piping — including the circular pipeThrough pin — lives in the
identity suite's pipe-integration.js.

The suite COMPLEMENTS WPT (`//src/wpt:streams`). The C++ seeds in
piping/error-propagation-forward largely root-cause to harness shapes
plus ledger #5/#6 below; piping/close-propagation-backward and
error-propagation-backward are DISABLED for hangs — this suite covers
that territory with BOUNDED observations (a pinned 'pending' outcome is
a deliberate defect pin, not a hole).

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | non-byte chunks piped into native identity | pipe rejects 'This WritableStream only supports writing byte types.' | STRINGS are UTF-8 ENCODED and pass through; a NUMBER chunk stalls the pipe silently — the next read pends forever (bounded defect pin) | `pipeThroughJsToInternal` |
| 2 | writable lock after a completed pipeThrough | stays locked; getWriter throws | getWriter() SUCCEEDS; `.locked` is transient/racy (observed both values) — only getWriter pinned | `pipeThroughJsToInternalCloses` |
| 3 | write-after-close rejection message | 'This WritableStream has been closed.' | 'Cannot write to a stream that is closing or closed' (`CLOSED_WRITE_MSG` helper) | `pipeToInternalToJsSimple`, `pipeToInternalToJsClose` |
| 4 | ws.close() queued BEFORE pipeTo | pipe locks both ends, waits, cancels source with 'This destination writable stream is closed.', then RESOLVES (see the TODO(conform) in the test) | pipe REJECTS IMMEDIATELY 'Destination closed before the pipe completed', cancels source with the same error (preventCancel suppresses), locks never observed held | `pipeToJsToJsCloseQueuedDestination`(+`PreventCancel`) |
| 5 | pipeTo brand check on a broken `this` | THROWS synchronously (before the capture_async_api_throws wrapper; the WPT general.any seed); a real stream with a bad destination REJECTS | both reject (spec) | `brandChecks` |
| 6 | destination hwm 0 (never desires) | pipe writes an available chunk anyway — ignores desiredSize (the WPT 'dest never desires chunks' seed family) | never writes (spec) | `sourceErroredAfterChunkHwmZero` |
| 7 | source queue after a preventCancel'd failing pipe | the not-yet-written chunk remains readable | read-ahead already consumed it; a fresh read PENDS (bounded) | `destWriteThrowsMidPipePreventCancel` |
| 8 | dest controller error()s while the pipe waits on a read | HALF-PROPAGATES: cancels the source with the error but FULFILLS the pipe promise | rejects the pipe and cancels the source with the error (spec) | `destControllerErrorsMidPipe` |
| 9 | FixedLengthStream length violations via pipe | overflow: pipe NEVER SETTLES (bounded); underflow: never settles | overflow: rejects RangeError; underflow: never settles (parity of nonconformance) | `fixedLengthStreamPipeOverflow`/`Underflow` |
| 10 | already-closed source → already-closed dest | rejects TypeError (spec; the WPT multiple-propagation seed) | FULFILLS as a trivially complete pipe | `closedSourceToClosedDest` |

Parity worth noting (probed, pinned): the whole error-propagation-
forward core matrix (starts-errored rejection/hook IDENTITY on both
ends, preventAbort/preventCancel incl. TRUTHY coercion, dest stays
usable under preventAbort); option plumbing (getter order
[preventAbort, preventCancel, preventClose, signal], throwing-getter
identity with no locks taken, bad-signal TypeError); pipeThrough
locked-endpoint sync throws; custom error type/instance preservation;
cancel-propagation through native identity AND JS transforms (source
ends CLOSED, all locks release); external close()/abort() on a piped
(locked) destination rejects while the pipe proceeds; backward write-
error propagation with hook identity; backpressure through
pipeThrough().pipeTo() chains; stalled-dest read-ahead ≤ 3 with source
hwm 1 (contrast ledger #6); FixedLengthStream exact-length pipes;
closed source → live dest closes the destination.

## Hang discipline

Never leave a pipe with a live infinite source and a releasable stalled
write: releasing it creates an unbounded pump that starves the event
loop on BOTH implementations (120s bazel timeout). Wind down by erroring
the source FIRST, then releasing the write (`pipeStopsPullingWhenDestStalls`).

## Compatibility flags

| Flag | Pinned in main cells | Other cells |
| --- | --- | --- |
| `streams_enable_constructors` + `transformstream_enable_standard_constructor` (2022-11-30) | yes (JS-backed endpoints) | `piping-cpp-legacy`: JS ctors throw the flag-naming Error; NATIVE→NATIVE pipes (body ↔ IdentityTransformStream) work unflagged |
| `capture_async_api_throws` | ledger #5's bad-dest rejection shape | — |
| `pedantic_wpt` (dateless opt-in) | `piping-cpp-pedantic` cell | zero observable deltas on this suite's surface |
| others (nodejs_compat, getters-on-prototype, toString tag, backpressure fixup, spec-compliant writer) | as in the sibling suites | — |

## Module map

| Module | Coverage |
| --- | --- |
| `pipe-matrix.js` | migrated pipe-streams-test.js wholesale (35): pipeThrough + pipeTo across JS↔native in all directions, prevent* combos, pre-aborted and mid-read AbortSignals, tee'd pipes, queued-destination close (ledger #1-#4) |
| `api-surface.js` | brand checks (ledger #5), option getter order, throwing getters, invalid signal, locked pipeThrough endpoints |
| `error-propagation.js` | forward matrix (starts-errored × prevent* × truthy), hwm-0 dest (ledger #6), custom-error preservation (migrated from streams-error-edge-cases-test.js) |
| `close-propagation.js` | the WPT-disabled backward territory, bounded: external close/abort on piped dest, write-throw backward propagation (ledger #7), idle dest-controller error (ledger #8) |
| `flow-control.js` | backpressure chain (migrated from streams-backpressure-test.js), stalled-dest read-ahead bound |
| `interop.js` | cancel propagation ×2 (migrated from api/streams/streams-test.js), FixedLengthStream (ledger #9), pre-settled pairings (ledger #10) |
| `legacy-pipes.js` | the unflagged cell (flags table) |
| `data-volumes.js` | end-to-end pipe volumes: 1 MiB pipeTo JS→JS, 8 MiB pipeThrough chain, 1 MiB JS→identity with body readback, 1 MiB identity→JS with a concurrent writer — all byte-exact |

Consumed sources (deleted or shrunk): pipe-streams-test.js (deleted),
streams-error-edge-cases-test.js (−2), streams-backpressure-test.js
(−1), api/streams/streams-test.js (−2; partiallyReadStream and inspect
remain). The security regression files remain authoritative and
separate: identity-transform-stream-uaf, pipe-source-error-uaf,
pipe-write-special-buffer (SharedArrayBuffer/resizable shapes).
