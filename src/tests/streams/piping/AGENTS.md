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
| 1 | non-byte chunks piped into native identity | strings are UTF-8 encoded and pass through on both; the NUMBER chunk fails the pipe — C++ rejects 'This WritableStream only supports writing byte types.'; TypeScript surfaces the identity validation TypeError through the non-fatal write-rejection pipe path (dest aborted, downstream reads reject) | `pipeThroughJsToInternal` |
| 2 | writable lock after a completed pipeThrough | stays locked; getWriter throws | spec finalize: both locks release when the pipe settles — one macrotask after the output's done, `.locked` is deterministically false and getWriter() succeeds (the release cascade is not synchronized with the output's done delivery, so loop-exit-instant reads are unspecified; `.locked` and getWriter share one predicate and never disagree at an instant) | `pipeThroughJsToInternalCloses` |
| 3 | write-after-close rejection message | 'This WritableStream has been closed.' | 'Cannot write to a stream that is closing or closed' (`CLOSED_WRITE_MSG` helper) | `pipeToInternalToJsSimple`, `pipeToInternalToJsClose` |
| 4 | ws.close() queued BEFORE pipeTo | pipe locks both ends, waits, cancels source with 'This destination writable stream is closed.', then RESOLVES (see the TODO(conform) in the test) | pipe REJECTS IMMEDIATELY 'Destination closed before the pipe completed', cancels source with the same error (preventCancel suppresses), locks never observed held | `pipeToJsToJsCloseQueuedDestination`(+`PreventCancel`) |
| 5 | pipeTo brand check on a broken `this` | THROWS synchronously (before the capture_async_api_throws wrapper; the WPT general.any seed); a real stream with a bad destination REJECTS | both reject (spec) | `brandChecks` |
| 6 | destination hwm 0 (never desires) | pipe writes an available chunk anyway — ignores desiredSize (the WPT 'dest never desires chunks' seed family) | never writes (spec) | `sourceErroredAfterChunkHwmZero` |
| 7 | dest controller error()s while the pipe waits on a read | HALF-PROPAGATES: cancels the source with the error but FULFILLS the pipe promise | rejects the pipe and cancels the source with the error (spec) | `destControllerErrorsMidPipe`, `destErroringWaitsForInFlightWrite` |
| 8 | FixedLengthStream length violations via pipe | overflow: pipe NEVER SETTLES (bounded); underflow: never settles | overflow: rejects RangeError; underflow: never settles (parity of nonconformance) | `fixedLengthStreamPipeOverflow`/`Underflow` |
| 9 | already-closed source → already-closed dest | rejects TypeError (a C++ deviation: the spec's ordered shutdown conditions give closing-forward priority) | FULFILLS (spec; WPT multiple-propagation 'closed readable to closed writable' pins the fulfillment) | `closedSourceToClosedDest` |
| 10 | SharedArrayBuffer-backed views into CompressionStream | copies the shared bytes; round-trips | write path REJECTS TypeError 'The provided value is not of type (ArrayBuffer or ArrayBufferView)' (spec: BufferSource without [AllowShared]) — while its identity stream ACCEPTS the same views | `sabViewThroughCompressionRoundTrip` |
| 11 | abort with a backlog, destination hwm 3 | reads and writes one chunk at a time: only the in-flight write completes | reads while desiredSize is positive; the abort waits for the three writes made (spec) | `abortWithBacklogHighWaterMark` |
| 12 | source queue after a non-fatal write rejection (identity, preventCancel) | the chunk after the invalid one was already read and is dropped | every chunk after the invalid one stays readable | `invalidChunkWithBacklogEndsPipe` |
| 13 | the source's pull aborts the pipe while the pipe reads a chunk | chunk dropped; the destination's pending read rejects | chunk written before the destination is aborted (spec: read chunks are written) | `pipeToJsToNativeCancel` |
| 14 | a chunk reaches the source while an abort waits for an in-flight write (destination hwm 2, preventCancel) | not read while the write is in flight: the chunk stays in the source | the pipe's pending read takes it; it is written, and the pipe settles once that write has (spec) | `lateChunkDuringShutdownWait` |
| 15 | abort with nothing to write while the pipe waits on a read (preventCancel) | the pipe and its read stay pending and the source stays locked; the next chunk completes the read, which drops it, and the pipe then rejects | the pipe settles and releases the source; a later chunk stays readable (spec) | `lateChunkAfterIdleAbort` |
| 16 | the destination's lock when its reader sees the pipe's abort (the source errors, preventAbort false) | already released | still held: released once the pipe's abort of the destination has settled (spec: the pipe finalizes after its shutdown action); the read rejects inside the abort | `pipeToJsToInternalErroredSource`, `pipeThroughJsToInternalErroredSource` |

Parity worth noting (probed, pinned): the whole error-propagation-
forward core matrix (starts-errored rejection/hook IDENTITY on both
ends, preventAbort/preventCancel incl. TRUTHY coercion, dest stays
usable under preventAbort); option plumbing (getter order
[preventAbort, preventCancel, preventClose, signal], throwing-getter
identity with no locks taken, bad-signal TypeError); pipeThrough
locked-endpoint sync throws; a bad destination, an option getter that
locks it, or a shadowed `locked` fails without locking the source;
custom error type/instance preservation; cancel-propagation through native identity AND JS transforms (source
ends CLOSED, all locks release); external close()/abort() on a piped
(locked) destination rejects while the pipe proceeds; backward write-
error propagation with hook identity; backpressure through
pipeThrough().pipeTo() chains; stalled-dest read-ahead ≤ 3 with source
hwm 1 (contrast ledger #6); a pipe shut down with chunks buffered (abort,
source error, write failure, invalid identity chunk) writes nothing more
at hwm 1, and preventCancel leaves the unread chunks readable;
FixedLengthStream exact-length pipes; closed source → live dest closes
the destination. Parity of nonconformance: a chunk enqueued in the
abort's turn while the pipe waits on a read with no write in flight is
lost from both ends (`lateChunkInAbortTurnIsLost`; the spec writes it).

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
| `pipe-matrix.js` | migrated pipe-streams-test.js wholesale (35): pipeThrough + pipeTo across JS↔native in all directions, prevent* combos, pre-aborted and mid-read AbortSignals, tee'd pipes, queued-destination close (ledger #1-#4, #13) |
| `api-surface.js` | brand checks (ledger #5), option getter order, throwing getters, invalid signal, locked pipeThrough endpoints, lock safety when validation fails |
| `abort-signal.js` | the pipe's AbortSignal is an abort algorithm: a synthetic 'abort' event is ignored, a listener's stopImmediatePropagation() cannot block the abort, an abort after the pipe settles does nothing |
| `error-propagation.js` | forward matrix (starts-errored × prevent* × truthy), hwm-0 dest (ledger #6), custom-error preservation (migrated from streams-error-edge-cases-test.js) |
| `close-propagation.js` | the WPT-disabled backward territory, bounded: external close/abort on piped dest, write-throw backward propagation, idle dest-controller error and one with a write in flight (ledger #7) |
| `flow-control.js` | backpressure chain (migrated from streams-backpressure-test.js), stalled-dest read-ahead bound |
| `shutdown-backlog.js` | shutdown with chunks buffered in the source: abort, source error, invalid identity chunk; what is written and what stays readable (ledger #11, #12) |
| `shutdown-pending-read.js` | abort while the pipe waits on a read: a chunk arriving during the shutdown's wait, in the abort's turn, or after an idle abort (ledger #14, #15) |
| `interop.js` | cancel propagation ×2 (migrated from api/streams/streams-test.js), FixedLengthStream (ledger #8), pre-settled pairings (ledger #9) |
| `special-buffers.js` | SharedArrayBuffer-backed and resizable-buffer views through native and JS pipe endpoints (migrated from pipe-write-special-buffer-test.js, strengthened to content checks; ledger #10); the JS path delivers the very view uncopied, resizable buffers stay resizable |
| `legacy-pipes.js` | the unflagged cell (flags table) |
| `data-volumes.js` | end-to-end pipe volumes: 1 MiB pipeTo JS→JS, 8 MiB pipeThrough chain, 1 MiB JS→identity with body readback, 1 MiB identity→JS with a concurrent writer — all byte-exact |

Consumed sources (deleted or shrunk): pipe-streams-test.js and
pipe-write-special-buffer-test.js (deleted),
streams-error-edge-cases-test.js (−2), streams-backpressure-test.js
(−1), api/streams/streams-test.js (−2; partiallyReadStream and inspect
remain). The security regression files remain authoritative and
separate: identity-transform-stream-uaf, pipe-source-error-uaf,
identity-transform-stream-uaf and pipe-source-error-uaf.
