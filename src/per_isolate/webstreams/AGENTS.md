# src/per_isolate/webstreams/

TypeScript Streams implementation with a backend-blind reader layer and
two consumer backends behind the `StreamConsumer`/`ByteStreamConsumer`
fence. Authoritative docs are IN-SOURCE — read the file headers first.

Parent directory conventions (primordials discipline, JSG capture trap,
private-brand dispatch, no `instanceof`) apply here — see
`src/per_isolate/AGENTS.md`.

## FILE MAP

| File          | Role                                                                                         |
| ------------- | -------------------------------------------------------------------------------------------- |
| `ring-buffer.ts` | O(1) FIFO with indexed access backing every internal queue below; leaf module             |
| `view-extent.ts` | A caller-supplied view's byte extent through captured getters; detached/out-of-bounds views (DataViews included) read as empty; leaf module |
| `queue.ts`    | QUEUED backend: single-queue/multi-cursor, JS sources; fence interfaces; invariant list      |
| `native.ts`   | NATIVE backend: C++-backed pull conduit; **the C++/JS contract** + invariants                |
| `readable.ts` | Reader layer + queued controllers + the BACKEND-DISPATCH points (constructor, tee, chains, byte-capable gate, JS-to-C++ extraction) |
| `writable.ts` / `transform.ts` / `strategies.ts` | WHATWG writable/transform/strategies                                     |
| `identity.ts` | IdentityTransformStream and FixedLengthStream (byte-capable identity transforms)             |
| `compression.ts` | CompressionStream/DecompressionStream over the C++ codec handle (utils.newCompressionCodec) |
| `encoding.ts` | TextEncoderStream and TextDecoderStream (pure JS codec transforms)                           |
| `streams.ts`  | Module aggregator (user-visible classes + the flag-gated DrainingReader)                     |
| `types.d.ts`  | TypeScript type definitions for the streams API                                              |

## THE QUEUED TEE MODEL (deliberate spec divergence)

The spec's `tee()` gives each branch its own controller and queue, fed by a
reader on the original. The queued backend instead has ONE queue with N
consumers (cursors), one per live branch: `tee()` forks the stream's cursor
into two branch cursors, removes the original's, and leaves the original a
permanently locked, inert shell — it consumes nothing, and there is no
per-branch controller to close or error. A tee of a branch does the same to
the branch: its two branches join the queue as consumers alongside their
aunt. Consequences, all handled by the controller (`readable.ts`,
`controllerConsumerLeaving`):

- The source is cancelled only when the LAST consumer leaves the queue
  (cancelled, or errored through the Node.js interop hook before the
  source requested close), with the reason of every consumer that left —
  one reason as is, several as an `AggregateError` in the order they left
  (the spec passes `[reason1, reason2]`). A consumer that leaves while
  others remain gets a promise settled with that cancel, or with
  `undefined` once the source closes or errors on its own (the spec's
  shared cancel promise). A branch that errors alone once close is
  requested — through the hook, or a byte branch's fractional-element fill
  at close — leaves without a reason and never cancels the source (the
  spec never forwards a branch's error to it); if it was the last
  consumer, the source ends as when every consumer has drained
  (`controllerConsumerErrored`).
- Erroring the source's own stream (its controller's `error()`, the
  interop hook) errors every consumer; erroring a live branch errors that
  branch alone.
- The source's own stream, once teed away, is closed by the source's own
  events, never by the branches' progress: it closes when close is
  requested (having no cursor, it has nothing to drain — so before any
  branch has read, while chunks may still be buffered for them), or when
  the last consumer's leaving cancels the source; it errors when the
  source errors. That is what `finished()` on a teed source observes (the
  spec's source closes once the tee's reader has drained it, or both
  branches have cancelled). The source itself ends later — errored,
  cancelled, or closed with every consumer drained — and that, not its
  stream's state, gates the controller's `error()`: an `error()` after
  `close()` still errors the branches that have chunks to drain, although
  the source's stream has already closed. `addAbortSignal()` on that
  stream, which stops listening once its stream has finished, no longer
  reaches them by then, and the interop hook is a no-op on it as on any
  stream that is no longer readable.
- A branch that has itself been teed is not the controller's stream, so
  none of the source's events would reach it: the tee closes it
  (`closeReadableStreamHusk`), and erroring it does nothing.
- Consumers that are collected rather than cancelled (every branch dropped
  while the source still holds its controller) leave the queue with no
  consumer for good: it drops what it holds and what is enqueued later,
  `desiredSize` reads as the high-water mark, and the controller releases
  the source — no more pulls, and its cancel never runs, since GC timing
  runs no user callback — while keeping its own state machine (`close()`
  closes the source's stream; `enqueue()` after it throws as ever). C++
  does the same but keeps pulling (readable ledger #20). Suite:
  `gc.js` in the readable and readable-byte suites.
- Backpressure follows the SLOWEST consumer: `desiredSize` is the
  high-water mark minus the largest backlog among the cursors. This trades
  flow for bounded memory: the spec's per-branch queues keep a reading
  branch flowing but buffer without bound for an idle one, while here a
  source that enqueues only while `desiredSize > 0` stalls every branch
  once an idle branch's backlog reaches the high-water mark, until that
  branch reads or leaves. The pull trigger is unaffected: a pending read
  on any cursor triggers a pull regardless of `desiredSize` (a `pull()`
  that itself gates on `desiredSize` stalls the same way). C++ behaves the
  same. Suite:
  `teeBackpressureFollowsSlowestBranch` in the readable suite.
- The controller's `byobRequest` covers a cursor's reads only while that
  cursor is the queue's only one. The exception is a released reader's
  head pull-into on the sole cursor at `tee()`/detach: the controller takes
  it over (`#releasedHead`), so a request the source holds across the fork
  keeps working, as in the spec. Responding to it enqueues the head's
  bytes, old and new, for every cursor (each drops its own copy of the old
  ones, from `adoptReleasedBytes`). `enqueue()`, `error()`, cancel and a
  closed-state `respond(0)` retire it. Suite: readable-byte ledger #25.
- Nothing walks a tree of streams: closing, cancelling and erroring act on
  cursors and their owners, and no stream retains another.

## HANDED-OFF STREAMS

A stream whose data moves to another stream is closed at the handoff,
left locked and disturbed, and drops its controller
(`closeReadableStreamHusk`): a teed branch (above), the source of a
native-backed tee, and a native-backed stream whose source is extracted
(a native-to-native pipe, the C++ bridge's `pumpTo`) or detached (the
bridge's `detach()`: `new Request(request)`, socket upgrades). The legacy
C++ streams leave the same streams locked, disturbed and closed. The
Node.js interop closed-promise settles then, and the interop error hook,
which acts only on a readable stream, cannot reach the moved source. A
queued stream detached from its source is the exception: it stays the
controller's stream, closing and erroring with the source, and the hook
on it errors the detached stream. Suites:
`src/tests/node/stream/finished-and-abort.js`,
`src/tests/streams/sockets/socket-streams.js`.

## KEY RULES

- The reader layer must stay backend-blind; backend divergence is
  confined to the fence interface and the marked BACKEND-DISPATCH points.
- Internal code never dispatches through a user-reachable prototype
  method: call the private method (`#error`, `#read`), a module slot
  (`controllerError`), or an `uncurryThis` capture taken at load time
  (`transform.ts`, `encoding.ts`). The public method is the brand check
  plus that internal entry.
- Every dictionary the implementation builds itself — the sources, sinks,
  strategies and transformers of the internal pairs, and the stand-in for
  an omitted argument (`kEmptyDictionary`) — is null-prototype, so a
  polluted `Object.prototype` cannot supply members. Members of the C++
  native source are read with `declaresMember`, which stops before
  `Object.prototype`. Copies of internal bytes use `cloneArrayBuffer`
  (spec CloneArrayBuffer), never the species-consulting `slice`.
  Suite guard: each suite's `pollution.js`.
- Do not port logic across the fence without checking BOTH invariant
  lists (`queue.ts` and `native.ts` headers).
- The native source contract (marker symbol, standard pull/cancel hooks,
  byobRequest discrimination, once-per-pull delivery, per-pull abort
  signal for cancellation, under-delivery = EOF signal delivering the
  partial as `{done: false, value: partial}` with the next read
  observing EOF, tee hook, `expectedLength` exact-total byte contract)
  is specified in the `native.ts` header.
  The C++ implementation (`ReadableStreamNativeSource` in
  `src/workerd/api/js-readable-stream.{h,c++}`) MUST conform to it; JS
  mocks in tests exercise the conduit independently. Key addition:
  `pull` receives an extension `signal`
  argument — the source checks `signal.aborted` before delivery and stashes
  bytes for redelivery if aborted (race buffering lives source-side; the JS
  conduit is uniformly bufferless).
- `nativeStreamInternals` (markers, extraction symbols, conduit
  construction) is module-private, consumed only by readable.ts/writable.ts
  and the C++ bridge via the API-symbol registry. The C++ mocks in
  `js-readable-stream-test.c++` construct real `ReadableStreamNativeSource`
  objects; no JS-visible marker export exists.
- Every stream instance carries an own, non-enumerable api-symbol brand
  (`kReadableStreamBrand` / `kWritableStreamBrand`), stamped at the very
  top of the constructor (before any early return). The C++ bridge's
  `tryUnwrapTs` recognizes streams by probing it — an own-data-property
  read that executes no JS. That constraint is load-bearing: recognition
  runs during RPC deserialization, inside V8's no-JS-execution scope. Any
  new construction path MUST go through the constructors (or stamp the
  brand itself).
- The C++ bridge MUST NOT dispatch into the TypeScript implementation
  while JS execution is disallowed (`js.isJavascriptExecutionDisallowed()`,
  set during RPC value deserialization): `getCppExport`/`dispatchCall`/
  `invokeMethod` assert this. Bridge operations reachable in that scope
  either answer from C++-side knowledge (state probes return the
  hydration-fresh answers; see `JsReadableStream::isDisturbed`) or happen
  before the scope entirely (stream construction via
  `RpcDeserializerExternalHandler::prepare()`'s externals hydration).

## NODE.JS INTEROP HOOKS

`ReadableStream.prototype` and `WritableStream.prototype` carry two
non-enumerable members keyed by the well-known symbols Node's own web
streams use: a `Symbol.for('nodejs.webstream.isClosedPromise')` getter
(an object whose `promise` settles with the stream: fulfilled on close,
rejected with the stored error, created lazily and marked handled) and a
`Symbol.for('nodejs.webstream.controllerErrorFunction')` method (errors
the stream as its controller's `error()` would; a native-backed readable
also cancels its source; a queued tee branch, whose controller is shared
with its siblings, errors alone — its cursor leaves the queue as a
cancelled branch's would, until the source requests close; a branch that
has itself been teed is inert; see the tee model above). The readable method errors byte streams too, native
ones included; Node's is deliberately a no-op for byte stream controllers,
so `addAbortSignal()` on a `Response` body (a byte stream in Node) is
inert there and errors the body here. The method runs neither the sink's
abort nor the source's cancel, so on a half of a transform pair
(`transform.ts`, `identity.ts`, `compression.ts`; the encoding streams
are TransformStreams) the other half would never learn of it: each pair
registers an interop error hook on both halves (`setInteropErrorHook` in
`internalsForPipe` and `internalsForTransform`), called synchronously
after the hook has errored a half, and errors the pair as
`TransformStreamDefaultController.error()` would (`TransformStreamError`).
Node errors the one half only and the other's pending operations stay
pending. `src/node`'s `finished()`/`eos()` (also behind
`stream/promises`) and `addAbortSignal()` rely on the hooks to observe or
error a web stream without taking its lock; nothing else in the node layer
touches them. The C++ implementation has no equivalent, and the node layer
raises `ERR_WEB_STREAM_INTEROP_UNSUPPORTED` there. Suite:
`src/tests/node/stream/finished-and-abort.js` and
`abort-transform-pairs.js`.

## ANTI-PATTERNS

- **NEVER** expose internals on user-visible exports. `streams.ts` exports
  exactly the user-visible classes plus `ReadableStreamDrainingReader`,
  which `main.ts` installs only under the internal-testing
  `expose_draining_reader` flag. (The Node.js interop hooks above are the
  one deliberate, symbol-keyed exception.)
