# ReadableStream({type:'bytes'}), ReadableByteStreamController, BYOB readers

Informal specification of byte-oriented ReadableStreams as implemented
in workerd, derived from — and kept in lockstep with — this suite.
**The tests are the normative artifact.** Value streams live in
readable/; the pipeTo/pipeThrough matrix belongs to piping/.

The suite COMPLEMENTS WPT (`//src/wpt:streams`). Probing showed the C++
failures in readable-byte-streams/* root-cause to a few construction and
pump divergences plus the close-with-partial and read-min shapes below;
the releaseLock→second-reader cluster and the buffer-hazard families are
behavior-parity (messages aside).

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | size() in a byte stream's strategy | silently accepted and ignored | RangeError (spec) — root of the WPT ctor seed | `sizeStrategyForBytes` |
| 2 | autoAllocateChunkSize 0/-1/NaN | TypeError 'cannot be zero.' (all three) | TypeError 'must be a positive integer' | `autoAllocateChunkSizeValidated` |
| 3 | pull counts (hwm 1, enqueue-in-pull) | 1,1,3 (readable ledger #4 mirror) | 1,2,3 (spec) | `pullCountShape` |
| 4 | sync start() throw | captured; stream errored (readable #6 mirror) | escapes constructor (spec) | `syncStartThrow`, `jsSourceError` |
| 5 | byobRequest on DEFAULT read, no autoAllocate | auto-allocates anyway: view(4096), or view(16384) under the UPDATED_AUTO_ALLOCATE_CHUNK_SIZE autogate (@all-autogates) | null (spec) — the subject of the streams_no_default_auto_allocate_chunk_size flag cell | `byobRequestOnDefaultRead` |
| 6 | Body-pump reads | pump fills byobRequest (BYOB reads) | pump pulls with byobRequest NULL even WITH autoAllocateChunkSize (direct default reads DO synthesize it) — respond-driven sources need dual paths | `bodyPumpByobRequestPresence` |
| 7 | close() with partially-filled read(view) | close succeeds; read resolves EMPTY view done=FALSE; closed fulfills | TypeError 'Insufficient bytes to fill elements in the given view' from close(), read, and closed (spec) | `closeWithPartiallyFilledView` |
| 8 | enqueue of detached/zero-length chunk | TypeError 'Cannot enqueue a zero-length ArrayBuffer.' | TypeError 'chunk must have a non-zero byteLength' | `enqueueDetachedBuffer`, `enqueueChunkMultipleTimesBytes` |
| 9 | released pending read's rejection | 'This ReadableStream reader has been released.' | 'This reader has been released' | `relockRespondRoutesToSecondReader` |
| 10 | respond(N) overflowing the current read's smaller view | RangeError 'Too many bytes [N]...'; second read stays pending | accepted; commits to the released descriptor; second read fulfills its view UNTOUCHED (zeros) | `relockRespondOverflowSecondView` |
| 11 | read min validation | min=0 TypeError; min>view TypeError | min=0 TypeError (other msg); min>view RANGEError | `readMinValidation` |
| 12 | close() below min with partial bytes (WPT read-min disable root) | read fulfills partial, done=false | read PENDS FOREVER while closed fulfills — BOTH nonconforming (spec: TypeError) | `closeBelowMin` |
| 13 | readAtLeast/min at native end-of-stream | below-min tail delivered done=false, then an extra read resolves done + empty view | done=true folded into the final below-min bytes | `readAtLeastByobReader` |
| 14 | tee cancel composite | pair-completing branch's reason only (readable #11 mirror) | AggregateError[r1, r2]; lone-branch cancel PENDS — never await it | `teeCancelComposite` |
| 15 | respondWithNewView with a different element size | adopts the NEW view's element size (6 bytes at once) | keeps the ORIGINAL read view's element size (4-byte multiple), queues the remainder | `readableStreamByteRespondWithNewViewUsesNewElementSize` |
| 16 | invalidated byobRequest message | 'This ReadableStreamBYOBRequest has been invalidated.' | 'This BYOB request has been invalidated' | `readableStreamByteRespond` |
| 17 | default-read delivery of a multi-chunk queue | COALESCES all queued chunks into one read | chunk-by-chunk (spec) | `byteDesiredSizeAccounting` |
| 18 | buffer-hazard messages (read detached view, respond after view detach, respondWithNewView foreign buffer, WASM Memory) | own texts | own texts (behavior parity everywhere) | `buffer-lifecycle.js` |
| 19 | close() with a pending UNFILLED BYOB read | read resolves done with an empty view | read PENDS FOREVER while close() succeeds (bounded; the #12 defect family without any min) — drain loops must close WITH the last enqueue, never against a parked empty read | `closeWithPendingUnfilledByobRead` |

Parity worth noting (probed, pinned): byte hwm defaults to 0 with NO
automatic pull; pull-throw and error-then-throw identity; enqueue
discards the outstanding byobRequest; read-after-close resolves done
with an empty view over a same-sized buffer (main cells); read(view)
detaches the caller's buffer at call time on JS-BACKED streams in every
era (see flags below); the whole releaseLock→second-reader cluster
(respond, respond(1)×2 Uint16 assembly, respondWithNewView,
autoAllocate respond/enqueue); staged min-fulfillment and min-met
reads; {min}-shaped arg ignored by default readers; readAtLeast exists
on BOTH implementations; tee CLONES chunks per branch (fresh buffers,
original detached, no cross-branch mutation) and propagates the same
error object to both branches; resizable ArrayBuffers usable on both
ends (enqueue detaches → resize throws); WebAssembly.Memory rejected
everywhere; the BYOB view-type matrix (byob-reader.js, migrated); GC
liveness of pending BYOB reads and byobRequests; SELF round-trips with
BYOB consumption, readAtLeast on echoed bodies, Response.bytes().

The ts cell additionally sets the internal-testing
`expose_draining_reader` flag, installing the
`ReadableStreamDrainingReader` global — the bulk-drain conduit the C++
bridge drives to consume TypeScript streams (conduit basics in the
identity suite's draining-reader.js). No such global exists under the
C++ implementation; `draining-reader.js` asserts both sides.

## Compatibility flags

| Flag | Pinned in main cells | Other cells |
| --- | --- | --- |
| `streams_enable_constructors` (2022-11-30) | yes | `readable-byte-cpp-legacy`: byte ctor throws the flag-naming Error; native-body BYOB reads still work |
| `streams_byob_reader_detaches_buffer` (2021-11-10) | yes — NATIVE streams only | `readable-byte-cpp-nodetach` (flag off): native read(view) keeps the CALLER's buffer (result view aliases it); JS-backed streams detach unconditionally in every era |
| `internal_stream_byob_return_view` (2024-05-13) | yes — NATIVE streams only | legacy + nodetach cells: native done-reads resolve value UNDEFINED; JS-backed done-reads resolve an empty view in every era |
| `streams_no_default_auto_allocate_chunk_size` (experimental, dateless) | no (ledger #5 is the default behavior) | `readable-byte-cpp-no-auto-allocate`: byobRequest null for default reads without autoAllocateChunkSize (the TS/spec behavior) |
| `pedantic_wpt` (dateless opt-in) | `readable-byte-cpp-pedantic` cell | zero observable deltas on this suite's surface |
| others (nodejs_compat, transform ctor, backpressure fixup, async-throws capture, prototype accessors, toString tag, spec-compliant writer) | as in the readable suite | — |

## Module map

| Module | Coverage |
| --- | --- |
| `construction.js` | ledger #1, #2, #4; byte hwm default 0 |
| `pull-timing.js` | ledger #3; pull-throw seeds |
| `controller.js` | ledger #5, #7; enqueue-discards-request; read-after-close; detach-at-call |
| `byob-reader.js` | view-type matrix + offsets + auto-allocate sizing (migrated streams-byob-edge-cases) + mismatched sizes/types, subarray, multi-pending-reads, byobreaderRegression (migrated streams-js-test) |
| `respond.js` | ledger #6, #8, #15, #16; all 31 streams-respond-test tests (respond/respondWithNewView/pumps/cancel races/UAF shapes) + js-test respond family |
| `release-relock.js` | ledger #9, #10; the WPT releaseLock→second-reader cluster |
| `read-min.js` | ledger #11-#13; byobMin/constraints/readAtLeast (migrated streams-test.js); /chunked SELF endpoint |
| `tee.js` | ledger #14; clone-per-branch; migrated byte-tee pair; error propagation |
| `buffer-lifecycle.js` | ledger #18; resizable ArrayBuffers; WASM Memory |
| `gc.js` | pending BYOB read + byobRequest survive gc() |
| `integration.js` | BYOB round-trips via SELF; readAtLeast on echoed body; bytes() |
| `js-compat.js` | ledger #17; byte halves of the mixed streams-js-test tests (closed promise, cancel reads, locked ops, globals) |
| `flag-no-auto-allocate.js` | the flag cell (migrated streams-no-auto-allocate-test) |
| `legacy-constructors.js` / `legacy-nodetach.js` | the flags table's legacy windows |
| `draining-reader.js` | TS only (C++ cell asserts the global's absence): a queued byte backlog plus the close sentinel swept in one batched read with chunks INTACT (no coalescing); the conduit drives pull with byobRequest null (ledger #5/#6 shape); expectedLength undefined; error/cancel propagation |
| `data-volumes.js` | byte-transfer volumes 64 B / 64 KiB / 1 MiB / 8 MiB via default and BYOB readers (incl. mismatched view/enqueue granularity), continuous prime-modulus pattern verified byte-exact; the source closes WITH its last enqueue (see ledger #19) |

Consumed sources (deleted): streams-js-test.js (value halves were
already covered by the readable suite), streams-tee-edge-cases-test.js,
streams-respond-test.js, streams-byob-edge-cases-test.js,
streams-no-auto-allocate-test.js. streams-test.js shrank to its
writable/TransformStream remnant. The security regression files remain
authoritative and separate: autovuln-37/60/131/132/148/319,
streams-byte-cancel-uaf, streams-byte-handlePush-uaf,
streams-byob-close-reentry, streams-byob-concurrent-readatleast,
streams-internal-read-buffer-gc, streams-circ-ref-regression,
streams-consumer-reentry-gc.
