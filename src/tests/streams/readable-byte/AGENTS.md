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
| 1 | size() in a byte stream's strategy | silently accepted and ignored; with an invalid highWaterMark too, that highWaterMark's TypeError (integer conversion) | RangeError (spec), before the highWaterMark is checked — root of the WPT ctor seed | `sizeStrategyForBytes` |
| 2 | autoAllocateChunkSize conversion (0, -1, NaN, 'a' and ±Infinity TypeError in both; a fraction truncates in both, so -0.5 is zero, and 1.5 and 3.9 give a default read 1- and 3-byte views) | an int: those TypeError 'cannot be zero.'; 2 ** 31 and up TypeError 'Value out of range' | the spec's [EnforceRange] unsigned long long, then a zero check: zero TypeError 'cannot be zero', the rest 'must be a finite number from 0 to 2^53 - 1'; up to 2 ** 53 - 1 accepted, 2 ** 53 and up TypeError | `autoAllocateChunkSizeValidated`, `fractionalAutoAllocateChunkSize` (parity) |
| 3 | pull counts (hwm 1, enqueue-in-pull) | 1,1,3 (readable ledger #4 mirror) | 1,2,3 (spec) | `pullCountShape` |
| 4 | sync start() throw | captured; stream errored (readable #6 mirror) | escapes constructor (spec) | `syncStartThrow`, `jsSourceError` |
| 5 | byobRequest on DEFAULT read, no autoAllocate | auto-allocates anyway: view(4096), or view(16384) under the UPDATED_AUTO_ALLOCATE_CHUNK_SIZE autogate (@all-autogates) | null (spec) — the subject of the streams_no_default_auto_allocate_chunk_size flag cell | `byobRequestOnDefaultRead` |
| 6 | Body-pump reads | pump fills byobRequest (BYOB reads) | WITH autoAllocateChunkSize: same — the draining conduit's wait-read synthesizes the auto-allocate descriptor, so pump pulls carry a byobRequest (parity, pinned). WITHOUT it: pump pulls present byobRequest null (ledger #5's spec side) while C++ auto-allocates anyway — sources without autoAllocateChunkSize stay dual-path | `bodyPumpByobRequestPresence` |
| 7 | close() with partially-filled read(view) | close succeeds; read resolves EMPTY view done=FALSE; closed fulfills | TypeError 'Insufficient bytes to fill elements in the given view' from close(), read, and closed (spec) | `closeWithPartiallyFilledView`; on a tee branch — a read pending at close() or issued after it — it errors that branch alone: close() succeeds, the sibling receives every byte (`teeBranchFractionalCloseErrorsBranch`), and a sole remaining branch's error never cancels the source, which requested close (`teeSoleBranchFractionalCloseSkipsSourceCancel`); two branches holding one each both error alone (`teeBranchesFractionalCloseErrorEach`). A released read the source holds a request over (#25) is the source's own pull-into when its own stream made it before tee(): close() throws, as on an unteed stream, erroring every branch and retiring the request (`teeHeldByobRequestFractionalAtClose`; C++ closes and keeps the request). One taken over from a branch is that branch's read: close() succeeds, and the branch's own two branches error alone, retiring the request (`teeHeldByobRequestFromBranchFractionalAtClose`; C++ keeps the request); on a detached body, the source's own stream or a tee branch, as undetached (`closeWithPartiallyFilledViewDetached`) |
| 8 | enqueue of detached/zero-length chunk | TypeError 'Cannot enqueue a zero-length ArrayBuffer.' | TypeError 'chunk must have a non-zero byteLength' | `enqueueDetachedBuffer`, `enqueueChunkMultipleTimesBytes` |
| 9 | released pending read's rejection | 'This ReadableStream reader has been released.' | 'This reader has been released' | `relockRespondRoutesToSecondReader` |
| 10 | respond(N) exceeding the second reader's smaller view (released 4-byte descriptor at head) | RangeError 'Too many bytes [N]...' validated against the SECOND read's view (a C++ deviation); second read stays pending | spec: the bounds check is against the HEAD descriptor — the respond is accepted, the released descriptor's bytes are enqueued, and the second read is served from the queue (2 of 3 bytes delivered, the third queued) | `relockRespondOverflowSecondView` |
| 11 | read min validation | min=0 TypeError; min>view TypeError | min=0 TypeError (other msg); min>view RANGEError | `readMinValidation` |
| 12 | close() below min with partial bytes (element-aligned) | read fulfills the partial bytes done=false; a subsequent read resolves done + empty view (the readAtLeast tail contract; the spec instead leaves the read pending until respond(0) commits { done: true, value: partial } — only a fractional fill makes close() throw, see #7) | same — the parked read settles one microtask after close(), handing its buffer back (transferred, not copied); its descriptor remains available for a later closed-state response | `closeBelowMin` |
| 13 | readAtLeast/min at native end-of-stream | below-min tail delivered done=false, then an extra read resolves done + empty view | same (the conduit's under-delivery commit; decided contract) | `readAtLeastByobReader` |
| 14 | tee cancel composite | pair-completing branch's reason only (readable #11 mirror) | AggregateError[r1, r2]; lone-branch cancel PENDS — never await it | `teeCancelComposite` |
| 15 | respondWithNewView with a different element size | adopts the NEW view's element size (6 bytes at once) | keeps the ORIGINAL read view's element size (4-byte multiple), queues the remainder | `readableStreamByteRespondWithNewViewUsesNewElementSize` |
| 16 | invalidated byobRequest message | 'This ReadableStreamBYOBRequest has been invalidated.' | 'This BYOB request has been invalidated' | `readableStreamByteRespond` |
| 17 | default-read delivery of a multi-chunk queue | COALESCES all queued chunks into one read | chunk-by-chunk (spec) | `byteDesiredSizeAccounting` |
| 18 | buffer-hazard messages (read detached view, respond after view detach, respondWithNewView foreign buffer, WASM Memory) | own texts | own texts (behavior parity everywhere) | `buffer-lifecycle.js` |
| 19 | close() with a pending UNFILLED BYOB read | read resolves done with an empty view | same — the parked read settles one microtask after close(), handing its buffer back; its descriptor remains available for a later closed-state response (#12's settlement without any min) | `closeWithPendingUnfilledByobRead` |
| 20 | remainder after a partial BYOB read, delivered to a DEFAULT read | copied into a fresh auto-allocated buffer (4096, or 16384 under the ledger #5 autogate), byteOffset 0 | view into the original enqueued buffer with its offset preserved (spec) | `partialViewThenDefaultRead` |
| 21 | byobRequest after a PARTIAL enqueue into a pending BYOB read | original request invalidated; no replacement exposed (null) | original request invalidated; a fresh request exposes a view shrunk to the remaining byte count (spec) | `cancelWithPartiallyFilledPull` |
| 22 | cancel() with a pending read (default, with or without autoAllocateChunkSize, read(view), readAtLeast; unfilled, or after a partial enqueue into a BYOB read; close() with a pending default read is #43) | read resolves done with an empty view; on a native body it rejects instead, with 'Stream was cancelled.' for a bare cancel() (the identity suite's ledger #20) | read resolves done with value undefined (spec), on a native body too — invisible to WPT, whose assert_object_equals equates an empty view with undefined | `cancelWithPartiallyFilledPull`, `cancelPendingReadsByteReaders` |
| 23 | error() while a close() is still pending (bytes queued) — readable #18 mirror | ignored: desiredSize already 0, the bytes drain to a clean close for default and BYOB readers | desiredSize is hwm minus the queued bytes (-2) until the error, then the stream errors: bytes discarded, default/BYOB reads and closed reject | `errorAfterCloseWithQueuedBytes` |
| 24 | pipeTo() from an autoAllocate tee branch, aborted (preventCancel) with its read pending | pipe stays pending until a chunk arrives, which the aborted pipe's read consumes and drops (bounded) | pipe rejects at once; the branch's next reader receives the next chunk | `teePipeAbortReleasesPendingRead` |
| 25 | byobRequest held by the source across tee() (reader released first) — the responded byte reaching both branches, or a sole remaining branch's read, across nested tees, via respondWithNewView and for auto-allocated reads, is parity (spec) | after respond() the request's view stays attached (zero-length), and the source's next enqueue() throws TypeError 'The byobRequest.view is zero-length or was detached'; bytes the released read already held are dropped (respond() delivers only the new byte, enqueue() only the chunk); error() leaves the view attached and respond() rejects as if closed; with both branches collected (#26) the request stays | spec: respond() invalidates the request and later chunks flow; the held bytes are delivered with the responded byte ([1,2,7]) or ahead of an enqueued chunk; error() invalidates the request. With both branches collected (#26), the request is retired, and none is minted over the held read again | `teeKeepsHeldByobRequest`, `teeHeldByobRequestWithReleasedBytes`, `teeHeldByobRequestAcrossNestedTee`, `teeHeldByobRequestAfterCloseOrError`, `teeHeldByobRequestBranchesCollected` (parity: `teeSoleBranchUsesHeldByobRequest`, `teeHeldByobRequestNewViewAndAutoAllocate`) |
| 26 | pull() after both tee branches are collected (controller held) — readable #20 mirror | keeps pulling for consumers that no longer exist; DEFECT: a source that enqueues on every pull runs until the stream closes | the source is released: pull() is never called again (the parity half — enqueue accepted, desiredSize at the high-water mark, byobRequest null, close() as ever — is `teeBranchesCollected`) | `teeBranchesCollectedPullStops` |
| 27 | where `closed` settles relative to the #12 tail read (close() below min with partial bytes) | read fulfills first, then closed | closed first — matching the order the spec gives a read that drains the last queued bytes after close(); that drain case is parity and is pinned alongside | `closedOrderAtEndOfData` |
| 28 | read(view) with a multi-byte view (e.g. Uint16Array) on a native body | resolves Uint8Array views of whatever bytes arrive, partial elements included | views of the read's own type holding whole elements; a partial element is carried into the next read, and one left at EOF errors the stream with TypeError 'Insufficient bytes to fill elements in the given view' (as a JS byte source's close() mid-element does, spec) | `nativeByobMultiByteViews` |
| 29 | a native body's reader released while its read is in flight, then tee() or clone(), or a failure of the body | releaseLock() throws TypeError (outstanding read promises); the read rejects with the body's error | the read rejects; both branches receive the whole body, including the in-flight read's bytes; the body's failure errors the stream at once: a second reader's closed rejects with the body's error, with or without a read waiting, and so does that read | `teeNativeBodyAfterReleaseMidRead`, `nativeBodyFailureAfterReleaseMidRead` |
| 30 | resizable ArrayBuffers handed in by read(view), enqueue() or respondWithNewView() | kept resizable except after enqueue(): the source can shrink byobRequest.view.buffer, and respond() then throws TypeError 'Cannot respond with a zero-length or detached view' (read left pending); respondWithNewView() and closed-stream read(view) results are resizable | transferred to fixed length (spec TransferArrayBuffer): byobRequest.view.buffer.resize() throws TypeError, and every result buffer is fixed-length | `resizableByobRequestCannotShrink`, `resizableBuffersDeliveredFixedLength`, `readResizableView` |
| 31 | default read with autoAllocateChunkSize set while bytes are queued (the ledger #17 shape with auto-allocation) | copies every queued chunk into one fresh autoAllocateChunkSize buffer (5 bytes in a 64-byte buffer) | hands over the head chunk, uncopied (spec PullSteps): chunk by chunk, each result over its enqueued buffer; only an empty queue allocates, for the source's byobRequest | `autoAllocateDefaultReadTakesQueuedChunk` |
| 32 | enqueue() meeting a released partial read's bytes (the next reader waiting; one reader or a tee branch) | a pending read(view) is filled with the released bytes alone ([1,2], then [3,4]); an auto-allocated default read gets them copied into its buffer | spec: the chunk is queued before BYOB reads are filled, so a pending read(view) takes both ([1,2,3,4]); a default read gets the released bytes alone as their own chunk (2-byte buffer); an element completed across the two is parity | `relockPartialHeadThenEnqueue`, `relockPartialHeadThenEnqueueShapes`, `teeReleasedPartialReadByob`, `teeHeldByobRequestEnqueueFillsByobRead` |
| 33 | when a promise returned by start() starts the stream (the readable suite's #23 for a byte source) | adopts it: the first pull runs before the first marker chained on it | spec (Node agrees): a new promise is resolved with it, so the pull runs after the second marker | `startPromiseSettledInNewPromise` |
| 34 | where a read(view)'s `then` lookup runs relative to the pull it triggers | answered by an enqueue(): after the enqueue()'s pull (the promise settles a microtask later); answered from the queue: no pull | spec: answered from the queue, the pull runs first (HandleQueueDrain before the chunk steps); answered by an enqueue(), the read is fulfilled before the enqueue() pulls | `byobReadThenLookupFollowsItsPull` |
| 35 | (no divergence) `expectedLength` conversion, as any `uint64_t` converts: a bigint must fit; anything else goes through ToNumber (`valueOf` runs once), must be finite and non-negative, and is truncated (`1.5`→1, `'5'`→5, `null`→0, `true`→1); above the uint64 maximum (`2n ** 64n`, `2 ** 65`) rejected; every failure a `TypeError`. The number `2 ** 64` is asserted under TypeScript only (a `TypeError`): C++'s range check lets it through to a cast whose result is undefined | same | same | `byteSourceExpectedLengthRejected`, `byteSourceExpectedLengthCoercion`, `byteSourceExpectedLengthRange` (parity) |
| 36 | (no divergence) a BYOB reader on a closed or errored stream with no controller (a branch of a closed native body's tee(), or the body new Request(request) makes of a closed or errored stream, a default stream's included) is accepted: the read resolves done with an empty view, or rejects with the stored error (the closed body itself, and a branch of a closed JS byte stream, which shares its controller, take one too) | same | same | `byobReaderOnSettledBranchOrDetachedBody` (parity) |
| 37 | a BYOB reader on a closed or errored default stream, or a branch of its tee() (which shares its controller) | accepted, as on any closed or errored stream: the read resolves done with an empty view, or rejects with the stored error | TypeError 'BYOB reader can only be used on a stream with a byte source' (spec: the reader requires a byte stream controller, whatever the stream's state; a stream with no controller takes one, #36) | `byobReaderOnSettledDefaultStream` |
| 38 | where `closed` settles relative to a readAtLeast() short of its minimum at a native body's end (#13) | the read fulfills with closed still pending, and the stream closes at the next read | closed first, then the read, as at a JS byte source's close() below min (#27). At a zero-byte end both resolve closed first, for read() and read(view) alike (parity, pinned alongside) | `closedOrderAtNativeEndOfData` |
| 39 | ctor argument conversion order, every member of a byte source (the readable suite's #2 for a default source) | the source first, every member in its own order (type, autoAllocateChunkSize, start, pull, cancel, expectedLength), then the strategy: hwm and its ToNumber, size | the strategy first (spec); then the source's members in lexicographic order (spec: autoAllocateChunkSize, cancel, expectedLength, pull, start, type) | `argumentConversionOrder` |
| 40 | which error wins when two are possible (the readable suite's #32; in both, spec: every source member converts before the strategy and a zero autoAllocateChunkSize are checked, so a non-callable pull, TypeError with different messages, beats an invalid highWaterMark, a size function and a zero autoAllocateChunkSize; a zero autoAllocateChunkSize is checked after an invalid highWaterMark, whose error wins, TypeError in C++ and RangeError in TypeScript) | a non-callable pull also beats a negative autoAllocateChunkSize, which the int conversion accepts and a later range check rejects | a negative autoAllocateChunkSize (TypeError) beats a non-callable pull (spec: it converts first) | `conversionErrorPrecedence` |
| 41 | a bare ArrayBuffer passed to enqueue() or respondWithNewView(), whose WebIDL parameter is an ArrayBufferView (any other non-view there, and every non-view at read(view) and readAtLeast(), is a TypeError in both, messages aside) | taken as a view over the whole buffer (the parameter is a BufferSource): enqueue() detaches it and delivers its bytes; respondWithNewView() accepts one of the request's buffer length, detaches it and delivers its bytes | TypeError 'chunk must be an ArrayBufferView' or 'view must be an ArrayBufferView' (spec); the buffer stays attached, nothing is enqueued, and the pending read is left for a later response | `nonViewArgumentsRejected` |
| 42 | a byte source's declared `expectedLength` as an exact total of the bytes it delivers through enqueue(), respond() and respondWithNewView(), a response to a byobRequest held across tee() (#25) included (delivering exactly the total, then close(), is parity) | not enforced: a delivery past the total reaches the reader, and close() short of it closes the stream | by design: the delivery past the total throws RangeError 'byte source delivered more bytes than its declared expectedLength', and close() short of it RangeError 'byte source closed before producing its declared expectedLength'; either errors the stream with that error | `byteSourceExpectedLengthContract`, `byteSourceExpectedLengthHeldRequest` |
| 43 | close() with a pending default read, with or without autoAllocateChunkSize (the close() half of #22's default reads; a BYOB read pending at close() is #19) | read resolves done with an empty view (a zero-length Uint8Array) | read resolves done with value undefined (spec: ReadableStreamClose runs the read request's close steps) | `closeWithPendingDefaultRead` |

Parity worth noting (probed, pinned): byte hwm defaults to 0 with NO
automatic pull; pull-throw and error-then-throw identity; enqueue
that fully fills a request discards the outstanding byobRequest;
read-after-close resolves done with an empty view over a same-sized
buffer (main cells), as does a read(view) pending at a native body's
end; read(view)
detaches the caller's buffer at call time on JS-BACKED streams in every
era (see flags below); the whole releaseLock→second-reader cluster
(respond, respond(1)×2 Uint16 assembly, respondWithNewView,
autoAllocate respond/enqueue, two pending reads released, a partially
filled head released), and a released tee-branch read taking no later
bytes; a released partial read's bytes reaching the next reader ahead
of later data, on one reader or a tee branch (pending BYOB/default read,
buffered, piped) and through a later tee() — how an enqueue() splits
them for the next reader is #32; staged min-fulfillment and min-met
reads; {min}-shaped arg ignored by default readers; readAtLeast exists
on BOTH implementations; tee CLONES chunks per branch (fresh buffers,
original detached, no cross-branch mutation) and propagates the same
error object to both branches; resizable ArrayBuffers usable on both
ends (enqueue detaches → resize throws; results' resizability is ledger
#30); WebAssembly.Memory and SharedArrayBuffer views rejected
everywhere; the BYOB view-type matrix (byob-reader.js, migrated); GC
liveness of pending BYOB reads and byobRequests; SELF round-trips with
BYOB consumption, readAtLeast on echoed bodies, Response.bytes().

The ts cell additionally sets the internal-testing
`expose_draining_reader` flag, installing the
`ReadableStreamDrainingReader` global — the bulk-drain conduit the C++
bridge drives to consume TypeScript streams (conduit basics in the
identity suite's draining-reader.js). No such global exists under the
C++ implementation; `draining-reader.js` asserts both sides.

## WPT map: readable-byte-streams/general.any.js expectedFailures

The 33 C++ expectedFailures in that WPT file (see
src/wpt/streams-test.ts), each mapped to the suite pin that owns the
divergence root. "Family" means the WPT test fails for the same root a
named suite test pins directly, differing only in incidental asserts.

| WPT test (abbreviated) | Root | Suite pin |
| --- | --- | --- |
| start() throws an exception | ctor captures sync start throws (ledger #4) | `syncStartThrow` |
| Automatic pull() after start() / after read() / after read(view) | proactive pull (ledger #3) | `pullCountShape` |
| autoAllocateChunkSize | auto-allocated byobRequest on default reads (ledger #5) | `byobRequestOnDefaultRead` |
| Respond to pull() by enqueue() asynchronously / multiple pull() by separate enqueue() / read() twice then enqueue() twice / Push source without pull signal / enqueue()+getReader()+read() | pull-count and coalescing family (ledger #3, #17) | `pullCountShape`, `byteDesiredSizeAccounting` |
| constructor rejects size with type "bytes" | ledger #1 | `sizeStrategyForBytes` |
| cancel() with partially filled pending pull() | partial-enqueue replacement request (ledger #21) and cancel-result shape (ledger #22) | `cancelWithPartiallyFilledPull` (direct) |
| getReader(), read(view), then cancel() | pull runs before cancel under C++ | `readViewThenCancelOrdering` (direct) |
| enqueue() with Uint16Array then read() | mismatched view/enqueue granularity | `readableStreamBytesMismatchedSizes`, `byobUint16Array` |
| read(view) Uint32Array filled by multiple enqueue() | partial fills across enqueues | `byobUint32Array`, `byobPartialRespondMisalignsFillOffset` |
| enqueue(), read(view) partially, then read() | remainder copied vs viewed (ledger #20) | `partialViewThenDefaultRead` (direct) |
| read(view) Uint16 on close()-d with 1 byte / errored if close()-d before fulfilling read(view) | close-with-partial (ledger #7) | `closeWithPartiallyFilledView` |
| Throwing in pull ignored if errored / pull throw errors stream | pull-throw shapes | `pullThrowIgnoredIfErrored`, `pullThrowErrorsStream` |
| enqueue() discards auto-allocated BYOB request | request invalidation | `enqueueDiscardsByobRequest` |
| releaseLock()+second-reader ×9 (respond / respond(1) Uint16 / respond(3) / respondWithNewView / autoAllocate ×3 / Uint16 respond(1) chains ×2) | the release-relock cluster (ledger #9, #10) | `release-relock.js` (whole module) |
| Multiple read(view): close() and respond() / big enqueue() / multiple enqueue() | multi-pending-read delivery | `readableStreamMultiplePendingReads` |

## Compatibility flags

| Flag | Pinned in main cells | Other cells |
| --- | --- | --- |
| `streams_enable_constructors` (2022-11-30) | yes | `readable-byte-cpp-legacy`: byte ctor throws the flag-naming Error; native-body BYOB reads still work |
| `streams_byob_reader_detaches_buffer` (2021-11-10) | yes — NATIVE streams only | `readable-byte-cpp-nodetach` (flag off): native read(view) keeps the CALLER's buffer (result view aliases it); JS-backed streams detach unconditionally in every era |
| `internal_stream_byob_return_view` (2024-05-13) | yes — NATIVE streams only | legacy + nodetach cells: native done-reads resolve value UNDEFINED; JS-backed done-reads resolve an empty view in every era |
| `streams_no_default_auto_allocate_chunk_size` (experimental, dateless) | no (ledger #5 is the default behavior) | `readable-byte-cpp-no-auto-allocate`: byobRequest null for default reads without autoAllocateChunkSize (the TS/spec behavior) |
| `pedantic_wpt` (dateless opt-in) | `readable-byte-cpp-pedantic` cell | zero observable deltas on this suite's surface |
| others (nodejs_compat, transform ctor, backpressure fixup, async-throws capture, prototype accessors, toString tag, spec-compliant writer, enable_weak_ref) | as in the readable suite | — |

## Module map

| Module | Coverage |
| --- | --- |
| `construction.js` | ledger #1, #2, #4, #39, #40; byte hwm default 0 |
| `pull-timing.js` | ledger #3, #33; pull-throw seeds |
| `controller.js` | ledger #5, #7, #21, #22, #23, #43; enqueue-discards-request; read-after-close and read-after-cancel; detach-at-call |
| `byob-reader.js` | ledger #20, #28, #31, #36, #37; view-type matrix + offsets + auto-allocate sizing (migrated streams-byob-edge-cases) + mismatched sizes/types, subarray, multi-pending-reads, byobreaderRegression (migrated streams-js-test) |
| `respond.js` | ledger #6, #8, #15, #16; all 31 streams-respond-test tests (respond/respondWithNewView/pumps/cancel races/UAF shapes) + js-test respond family |
| `release-relock.js` | ledger #9, #10, #29 (a native body's failure after a release mid-read), #32; the WPT releaseLock→second-reader cluster; release with two pending reads or a partially filled head |
| `read-min.js` | ledger #11-#13, #27, #38; byobMin/constraints/readAtLeast (migrated streams-test.js); read(view, options) options as a WebIDL dictionary (parity: null, a function's members, primitives rejected); /chunked SELF endpoint |
| `tee.js` | ledger #7 (on a branch), #14, #24, #25, #29, #32; clone-per-branch; migrated byte-tee pair; error propagation; released branch reads, incl. partially filled ones and tee() after a release; byobRequest held across tee(); tee() of a closed native body locking it |
| `buffer-lifecycle.js` | ledger #18, #30, #41; resizable ArrayBuffers; WASM Memory; SharedArrayBuffer-backed typed arrays and DataViews rejected by read(view), readAtLeast(), enqueue() and respondWithNewView() (TS also after close(), #19); values that are not views rejected by the same four, and converted before the method steps: a non-view at respondWithNewView() on an invalidated request, or at enqueue() after close() or error(), fails as one, as does a zero-length chunk there (parity) |
| `gc.js` | pending BYOB read + byobRequest survive gc(); both tee branches collected while the controller is held: enqueue() accepted, desiredSize at the high-water mark, byobRequest null, close() then enqueue() as ever (a parity pin of the observable surface — the retention checks are the readable suite's, a transferred buffer leaving nothing to WeakRef), one branch cancelled and the other collected, observed in the gc()'s own job (the readable suite's teeSurvivorBranchCollected, plus byobRequest null), and pull() stops (ledger #26); a byobRequest held across tee() is retired once both branches are collected (ledger #25); a closed, cancelled or errored stream releases its underlying source, and a source with only start() is released while its stream is still open (WeakRef-checked; parity) |
| `integration.js` | BYOB round-trips via SELF; readAtLeast on echoed body; bytes() |
| `js-compat.js` | ledger #17, #22; byte halves of the mixed streams-js-test tests (closed promise, cancel reads, locked ops, globals) |
| `flag-no-auto-allocate.js` | the flag cell (migrated streams-no-auto-allocate-test) |
| `legacy-constructors.js` / `legacy-nodetach.js` | the flags table's legacy windows |
| `draining-reader.js` | TS only (C++ cell asserts the global's absence): a queued byte backlog plus the close sentinel swept in one batched read with chunks INTACT (no coalescing); a read whose maxSize runs out at the last bytes before the close reaches the close all the same, reporting done and closing the stream in that call; with autoAllocateChunkSize the conduit's wait-read synthesizes the descriptor so pull carries a byobRequest (ledger #6); expectedLength undefined when undeclared, a declared one passed through as the exact bigint (above 2^53 and up to the uint64 maximum); error/cancel propagation |
| `data-volumes.js` | byte-transfer volumes 64 B / 64 KiB / 1 MiB / 8 MiB via default and BYOB readers (incl. mismatched view/enqueue granularity), continuous prime-modulus pattern verified byte-exact; the source closes WITH its last enqueue (single-shape loops; parked-read close settlement is ledger #19's) |
| `pollution.js` | prototype pollution neither implementation observes: a patched byte controller error() still errors the stream; readAtLeast() ignores a patched read(); tee copies and respond() remainders never consult the ArrayBuffer or %TypedArray% species; a native body ignores Object.prototype `type`/`autoAllocateChunkSize`; one Object.prototype.then getter fire per read, on an ordinary result object (default reads, autoAllocateChunkSize, BYOB and readAtLeast, at close, native bodies); ledger #34; a then getter that releases the reader while cancel() or close() settles its reads (default, autoAllocateChunkSize and BYOB reads, BYOB ones at a close() answered by respond(0) or not), or errors the source from a tee branch's read while close() settles that branch's reads: the next read still resolves done (parity) |
| `expected-length.js` | ledger #35, with the converted length observed as a fetch body's Content-Length via SELF; integral lengths up to the uint64 maximum accepted; ledger #42, the declared length as an exact total through enqueue(), respond() and respondWithNewView(), and through a byobRequest held across tee() |

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
