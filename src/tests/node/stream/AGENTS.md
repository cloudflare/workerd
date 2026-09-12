# node:stream × web streams

An informal specification of the `node:stream` web-interop surface —
`Readable.toWeb/fromWeb`, `Writable.toWeb/fromWeb`, `Duplex.toWeb/fromWeb`,
`Duplex.from`, `Readable.from` over a web stream, `pipeline`, `compose`,
`finished`, `addAbortSignal`, `node:stream/web`, `node:stream/consumers` —
derived from and kept in lockstep with the test suite in this directory.
**The tests are the normative artifact**; this document maps behaviors to
the tests that assert them. Every test runs against the C++ streams
implementation (`stream-cpp.wd-test`) and the TypeScript one
(`stream-ts.wd-test`); the divergence ledger pins the places where the
node layer observes a difference between them.

The implementation under test is `src/node/internal/streams_readable.js`
(`newReadableStreamFromStreamReadable`, `newStreamReadableFromReadableStream`),
`streams_writable.js` (`newWritableStreamFromStreamWritable`,
`newStreamWritableFromWritableStream`), `streams_duplex.js`
(`newReadableWritablePairFromDuplex`, `newStreamDuplexFromReadableWritablePair`,
`duplexify`), `streams_pipeline.js`, `streams_compose.js`,
`streams_end_of_stream.ts`, `streams_add_abort_signal.ts`, and
`src/node/stream/{web,consumers}.js`.

## Core semantics

### Readable.toWeb(readable, { strategy })

- The source is paused at adaptation and resumed by the web stream's
  `pull()`; `'data'` chunks are enqueued and the source is paused again
  whenever `desiredSize <= 0`. The strategy is derived from the source:
  `CountQueuingStrategy` for objectMode, `ByteLengthQueuingStrategy`
  otherwise, both with the source's `readableHighWaterMark`; an explicit
  `strategy` replaces it.
- Byte chunks are **copied**: the reader receives a plain `Uint8Array` over
  a fresh buffer (the source's `Buffer`, and any `Uint8Array` the source
  pushed — the Readable itself wraps those in a pooled Buffer). objectMode
  chunks pass by identity.
- Source end → stream closes; source error → stream errors with the same
  instance; source destroyed without error → `AbortError` whose `cause` is
  `ERR_STREAM_PREMATURE_CLOSE` — also when the source destroys itself from
  inside the `_read()` the adapter's `pull()` reaches through `resume()`.
  An `'error'` a legacy source emits after it has ended is swallowed by the
  no-op listener the bridge leaves behind.
- `reader.cancel(reason)` destroys the source with `reason` (an
  `AbortError` when none is given) — through `destroyer`, so a `pipeTo()`
  whose destination fails destroys the source with the destination's error.
  A `cancel()` issued from a user `'data'` listener that runs before the
  adapter's is quiet: the adapter's enqueue into the just-cancelled stream
  fails, and the failure is dropped (the source is already being destroyed
  with the reason) rather than escaping the `'data'` emission.
- A user strategy whose `size()` fails (throws, or returns NaN, a negative
  number, ∞) fails the enqueue and errors the stream with that error; reads
  reject with it. Under TypeScript the enqueue also throws (spec) and the
  adapter destroys the source with the error — where Node lets it escape
  as an uncaught exception per chunk; under C++ the enqueue swallows the
  failure and the source is left paused, alive (ledger #6).
- A destroyed or ended source, or a Duplex created with `readable: false`,
  yields an already-cancelled stream. A non-Readable is
  `ERR_INVALID_ARG_TYPE`. The web stream is constructed before the source
  is touched: a source misreporting its `readableHighWaterMark` (NaN,
  negative; ∞ under C++ — ledger #7) makes `toWeb` throw and leaves the
  source as it was, unpaused and without listeners; so does a getter that
  throws.

### Readable.fromWeb(stream, { highWaterMark, encoding, objectMode, signal })

- Options are validated before the (default) reader is acquired; the web
  stream stays locked for the Readable's lifetime. One `read()` per
  `_read()`: nothing is pulled until the Readable is read.
- `done` → `'end'`/`'close'`; a stream error (with or without a read in
  flight) → destroyed with the error instance; `destroy(reason)` cancels
  the stream with `reason` (`null` for a bare `destroy()`), never after the
  stream has already closed; `signal` abort → `AbortError`, and the stream
  is cancelled with it.
- Without objectMode only byte-like chunks are accepted (strings become
  Buffers, anything else errors with `ERR_INVALID_ARG_TYPE`); `encoding`
  decodes to strings. A chunk that cannot become a Buffer (a view over a
  detached ArrayBuffer) destroys the Readable with the conversion's
  `TypeError` and cancels the web stream with it — Node's adapter leaves
  such a stream hanging; so does a 'data' listener throwing during the
  delivery (the delivery runs inside the web read's promise). A chunk
  over a `SharedArrayBuffer` is delivered as a Buffer over that buffer,
  one over a resizable buffer aliases it until delivery, and zero-length
  chunks contribute nothing. The same holds for `Duplex.fromWeb`'s readable
  half, for `compose()`'s web tail (the composition fails) and for a
  `pipeline()` web source (the pipeline fails; the source is cancelled
  without a reason, by the pump's iteration leaving early, as in Node).

### Writable.toWeb(writable)

- Accepts anything with `write` and `on` functions; a non-node-stream duck,
  a destroyed or ended Writable, or a Duplex with `writable: false` yields
  an already-closed stream. A duck that also claims `writable: true` is
  adapted as a live legacy writable — `end-of-stream` subscribes to it
  with plain `on()` calls, whatever `on()` returns — and is taken at its
  word: a `writableNeedDrain` that stays true keeps a web write pending
  without calling `write()`, a truthy non-boolean `write()` return counts
  as accepted, a falsy one as backpressure. The strategy follows the
  writable: its `writableHighWaterMark` with the default size of 1 per
  chunk, or a `CountQueuingStrategy` in objectMode. The web stream is
  constructed before the writable is touched: a misreported
  `writableHighWaterMark` (NaN, negative) makes `toWeb` throw (ledger #7)
  and leaves the writable without listeners, so ending it afterwards
  finishes quietly.
- Chunks reach the sink as `Writable.prototype.write()` delivers them
  (Buffer identity, `Uint8Array` as a Buffer over the same memory, strings
  as UTF-8, objectMode by identity).
- Backpressure crosses through `'drain'`: a web write whose node `write()`
  returned false stays pending until the node side drains.
- `writer.close()` calls `end()` (unless the caller already has) and
  settles after `'finish'` — also when the node side was ended directly
  and its `_final()` is still running; a `_write` or `_final` error
  rejects the pending promises and `writer.closed` with the same instance
  (a synchronous `_write` error also rejects the write it belongs to); the
  node side finishing or being destroyed on its own, before any
  `writer.close()`, errors the stream with an `AbortError`;
  `writer.abort(reason)` destroys the writable with `reason` (an
  `AbortError` when none). Re-entered from inside a `_write()`: a
  `destroy(err)` there rejects the write in flight and `writer.closed` with
  `err` and the node side reports it once; a `writer.abort(reason)` there
  lets the write in flight resolve (the spec finishes an in-flight write
  before erroring), then rejects `closed` with `reason`. A web write of a
  chunk a byte-mode Writable cannot take (a number, a plain object) throws
  `ERR_INVALID_ARG_TYPE` inside the sink, erroring the stream only: the
  node writable, as in Node, stays intact and directly writable.

### Writable.fromWeb(stream, { highWaterMark, decodeStrings, objectMode, signal })

- Options validated before the writer is acquired; locked for the
  Writable's lifetime. Each `_write` awaits `writer.ready` then
  `writer.write(chunk)`; writes issued back to back (or corked) are batched
  through `_writev`, each entry's **chunk** written in order; a failed batch
  fails every callback in it with the sink's error and errors the Writable
  once.
- `end()` → `writer.close()` before `'finish'`; `destroy(err)` →
  `writer.abort(err)`; `destroy()` → `writer.close()`. A web-side error
  (even with no write issued) or a rejected sink `close()` destroys the
  Writable with that error. `decodeStrings: false` passes strings through;
  objectMode passes anything by identity. Chunks are handed to the sink by
  reference: a `Uint8Array` arrives as a Buffer over the caller's memory
  (views over a `SharedArrayBuffer` or a `WebAssembly.Memory` included),
  so a sink that transfers the chunk's buffer detaches the caller's view; a
  view over an already-detached buffer is refused synchronously by the
  Writable itself, as in Node.
- `ERR_STREAM_PREMATURE_CLOSE` (web side closing before the Writable ended)
  is unreachable while the adapter holds the writer: only `destroy()` can
  close it, and a destroyed Writable ignores a second destroy.

### Duplex.toWeb(duplex) / Duplex.fromWeb(pair, options)

- `toWeb` is the two adapters over the two halves; a destroyed Duplex or a
  missing side yields the corresponding pre-cancelled/pre-closed half. The
  readable half is a default (non-byte) stream. **Both halves observe the
  whole Duplex through end-of-stream**, so `writer.close()` settles only
  once the readable side has ended too, and the readable reports `done`
  only once the writable side has finished.
- `fromWeb` validates the pair with `instanceof` (an already-locked
  readable leaves the writer lock taken), defaults `allowHalfOpen` to
  **false** (readable EOF ends the writable side and closes the web
  writable), and on destroy aborts the writer and cancels the reader with
  the destroy reason (`null` when bare — unlike `Writable.fromWeb`, which
  closes). A web readable error destroys the duplex without aborting the
  web writable; a web writable error destroys it without cancelling the web
  readable. Consuming it to completion with `for await` destroys it with
  an `AbortError` (its writable half is not finished yet), aborting the web
  writable. A bare `destroy()` from inside `'data'` while a write is in
  flight cancels the readable at once and aborts the writable only once
  the sink has settled that write, whose callback reports success. A
  second `end()` from inside `'finish'` reports `ERR_STREAM_ALREADY_FINISHED`
  to its callback; `end(chunk)` there is a write after end (callback and
  `'error'`, which destroys the duplex), as in Node.

### Duplex.from(webStream)

- `Duplex.from()` over a lone web stream yields a Duplex whose missing side
  is marked finished/ended.

### pipeline

- `pipeline()`: a `ReadableStream` (or a `TransformStream`'s readable) is
  consumed through a reader the pump owns, its chunks passed on as they
  are (a promise-valued chunk stays a promise); a `WritableStream` (or a
  `TransformStream`'s writable) is fed through a writer that honors
  `ready`, closes at the end unless `end: false`, and aborts on failure —
  the pump keeps the writer, so a web destination stays locked. The
  pipeline's teardown (a signal abort, a failed stage) reaches the web
  stages through those handles: the source's reader is cancelled with the
  pipeline's error, which settles a pending read (a source the pump never
  started reading is cancelled too), and the destination's writer is
  aborted with it. A web destination's own failure — its controller
  erroring, a write rejecting — is observed independently of the source,
  so it fails the pipeline even while the source is idle; a web source's
  own failure is observed independently of the destination, so it fails
  the pipeline (destroying the destination) even while the pump waits on
  a node destination's backpressure — one that never drains would
  otherwise hang it. A failed web sink
  fails the pipeline and destroys the node source (its own abort algorithm
  does not run: the stream is already errored); a failed web source
  destroys the node destination; a failed node destination — even one
  failing while the web source is idle — cancels the web source. A web
  destination that is already locked fails the pipeline with the lock
  error and leaves the lock with its owner. A signal abort fails the
  pipeline with an `AbortError`, also when every stage is a web stream and
  idle; a pending sink write is allowed to settle first. `stream/promises`
  treats a trailing web stream as a destination.

### compose

- `compose()`: web streams are validated by position; a web head is
  written through its writer, a web tail read through its reader; a node
  tail's output is drained into the composed stream's own buffer as it is
  produced, so `end()` completes without a consumer. With a web tail the
  composed writable side finishes once the tail's readable side has closed
  AND the pipeline has completed (a tail's `close()` may close its readable
  at once yet settle later). Destroying a running composition fails its
  pipeline with the destroy error — a node tail is destroyed as a stage;
  with a web tail the pipeline is failed directly, whatever state its
  readable side is in — so every stage is destroyed with that error (the
  head, a web tail's writable side) and the composed stream reports it — an
  `AbortError` for a bare `destroy()` while the pipeline runs — then
  closes. A composition whose sides have both completed (the automatic
  destroy, a readable-only composition drained to its end) leaves its
  pipeline to complete and reports the pipeline's (clean) outcome.

### finished / addAbortSignal

- `finished()` and `addAbortSignal()` need the Node.js interop hooks (see
  ledger #5); `compose()` needs them only when its head is writable and its
  tail is a web stream, and refuses that shape before starting its
  pipeline or taking any lock. `addAbortSignal()` on one branch of a tee
  errors that branch alone: the sibling keeps its buffered chunks and its
  reads, and the source is cancelled only once every consumer is gone, with
  each one's reason; on a branch that has itself been teed it does nothing
  (the queued tee model's inert shell, see
  `src/per_isolate/webstreams/AGENTS.md`); a branch's `cancel()` promise
  settles with the source's cleanup in either order.

### Liveness

- The node stream keeps the web stream it was adapted to alive (`toWeb`,
  both directions, ephemerally: exactly as long as the node stream lives).
  The C++ streams implementation's controller does not keep its stream
  alive (readable ledger #19 in `src/tests/streams`), and the node side —
  which its own pending I/O keeps alive — holds only the controller: a
  full GC could otherwise collect a `toWeb` stream reachable only through
  a pending `writer.closed`/`write()` continuation (which then never
  settles), or leave a `Readable.toWeb` source pushing into a collected
  stream, never paused. The gc.js module forces GCs in that window.

### Prototype pollution

- The adapters, unlike Node's, use no primordials: they call the live
  `Promise.prototype.then` (and `Promise.all`, `withResolvers`, `finally`),
  so a patched `then` sees every hop (a three-chunk `fromWeb`/`fromWeb`/
  `toWeb` round trip makes 18 calls under C++, 15 under TypeScript — not
  pinned), and a `then` getter on `Object.prototype` is consulted when
  plain objects (the reader's `{ value, done }` results) are assimilated.
  A transparent patch changes nothing about the data. A `then` that throws
  during construction throws out of `fromWeb` (all three) with the web
  stream(s) left unlocked and usable — neither cancelled nor aborted — and
  the node stream, never handed out, destroyed quietly *before* the lock
  is released, so a `then` that registered the adapter's `closed` handlers
  before throwing leaves nothing behind: the release rejecting `closed`
  into those handlers finds the node stream already destroyed, and no
  `'error'` escapes from a stream nobody could listen to.

## Compatibility flags

`stream-cpp.wd-test` pins `streams_enable_constructors` (the toWeb adapters
construct `new ReadableStream()`/`new WritableStream()`, gated under C++)
and `transformstream_enable_standard_constructor` (the transform chains).
`stream-ts.wd-test` omits both; its variants prove the TypeScript
implementation is indifferent to them. `stream-cpp-pedantic.wd-test` runs
the full module set with `pedantic_wpt` added and asserts nothing changes.
The three cells pass `--expose-gc` to V8 for `gc.js`.

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `streams_enable_constructors` (2022-11-30) | toWeb adapters and `node:stream/web` constructors work | `legacyToWebHitsConstructorGate`, `legacyStreamWebConstructorsGated` |
| `transformstream_enable_standard_constructor` (2022-11-30) | `new TransformStream({ transform })` honors its transformer | `legacyTransformStreamIgnoresTransformer` |

## Divergence ledger (C++ vs TypeScript)

Every entry is asserted on both sides via `usingTsImpl`.

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | Locked-stream error text (`getReader`/`getWriter` on a locked stream, reached through fromWeb) | `This ReadableStream is currently locked to a reader.` / `This WritableStream is currently locked to a writer.` | `Cannot get a reader for a stream that is locked` / `Cannot get a writer for a stream that is locked` | `fromWebLocksTheStream`, `fromWebLockedInputThrows`, `writableFromWebLocksTheStream` |
| 2 | Write to a pre-closed `Writable.toWeb` stream | `TypeError` `This WritableStream has been closed.` | `TypeError` `Cannot write to a stream that is closing or closed` | `toWebDuckTypedInputYieldsClosedStream` |
| 3 | BYOB reader on a `toWeb` readable | `This ReadableStream does not support BYOB reads.` | `BYOB reader can only be used on a stream with a byte source` | `toWebReadableIsNotByteStream` |
| 4 | `FixedLengthStream` enforcement through `Writable.fromWeb` (identity ledger #11) | readable errors with `TypeError`; the node write and end succeed | write/close reject `RangeError`; the node Writable errors; readable errors with the same `RangeError` | `fromWebFixedLengthOverwrite`, `fromWebFixedLengthUnderwrite` |
| 5 | Node.js interop hooks (`Symbol.for('nodejs.webstream.isClosedPromise')`, `…controllerErrorFunction`) | absent; `finished()`, `promises.finished()`, `addAbortSignal()` and a writable-head/web-tail `compose()` throw `ERR_WEB_STREAM_INTEROP_UNSUPPORTED` up front | non-enumerable prototype getter and method; the APIs work as in Node, including on native-backed streams (a `Response` body) | `finished-and-abort.js`, `composeNodeHeadWebTail` |
| 6 | `Readable.toWeb` with a user strategy whose `size()` throws or returns an invalid size (readable ledger #8/#9 in `src/tests/streams`) | `enqueue` errors the stream without throwing (a `TypeError` "cannot be converted" for NaN/negative); the source stays paused and alive | `enqueue` errors the stream and throws (`RangeError` "Invalid chunk size" for NaN/negative); the adapter destroys the source with the error | `toWebLyingStrategyDestroysSource` |
| 7 | `toWeb` over a stream misreporting its high-water mark (readable ledger #3 in `src/tests/streams`) | `TypeError` "The value cannot be converted…" for NaN, negative and ∞ | `RangeError` "Invalid highWaterMark" for NaN and negative; ∞ accepted (spec) | `toWebInvalidHighWaterMarkLeavesSourceUntouched`, `toWebInvalidHighWaterMarkLeavesWritableUntouched` |

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | `node:stream/web` named and default exports are the globals; adapter statics on the classes and legacy aliases |
| `readable-to-web.js` | delivery; argument validation; byte copy / objectMode identity; derived and explicit strategies with the pause/resume they produce; end, error, premature-close propagation; cancel (with and without reason) and pipeTo-failure destroying the source; a failing user `size()` (ledger #6, nothing uncaught); cancel from an earlier `'data'` listener (quiet); an invalid high-water mark leaving the source untouched (ledger #7); a late `'error'` after end swallowed; destroy inside the pulled `_read()` → `AbortError`; unreadable inputs |
| `readable-from-web.js` | delivery; errored/erroring sources through async iteration; validation before locking; lock and locked-input errors (ledger #1); pull on demand; end/close ordering; errors with and without a read in flight; a detached-view chunk → `TypeError`, cancel with it; SAB chunk by reference and empty chunks skipped; a resizable chunk aliasing until delivery; a 'data' listener's throw erroring the Readable; destroy → cancel (reason, `null`, skipped after close); `encoding`, `objectMode`, `highWaterMark`, `signal` |
| `writable-to-web.js` | delivery; close → end → finish; pipeTo completion; sync and async node errors; `_final` error; `destroy(err)` and `writer.abort()` from inside `_write` (once, in-flight write settled per spec); a non-byte web chunk erroring the stream only; node-initiated end/destroy → `AbortError`; close after a direct end() waiting for a slow `_final` and rejecting with its error (sync and async), nothing uncaught; abort (with and without reason); validation; duck input and unwritable inputs → closed stream (ledger #2); a live duck (non-chaining `on()`, `needDrain` liar, truthy/falsy `write()` returns); an invalid high-water mark leaving the writable untouched (ledger #7); derived strategy; drain-driven backpressure; chunk conversion |
| `writable-from-web.js` | delivery; web error / sink rejection / close rejection destroying the Writable once with no unhandled rejection; back-to-back and corked writes through `_writev`; failed batch; validation before locking; lock (ledger #1); chunk conversion; by-reference hand-off (SAB and WebAssembly.Memory views, a transferring sink detaching the caller's view, a detached view refused); `decodeStrings`/`objectMode`; end → close; destroy → abort or close; writes complete on sink acceptance |
| `duplex-to-web.js` | pair round trip; validation; destroyed and half Duplexes; non-byte readable (ledger #3); destroy(err) erroring both halves; the whole-Duplex end-of-stream coupling of the halves |
| `duplex-from-web.js` | a detached-view chunk destroying the duplex (`TypeError`; readable cancelled and writable aborted with it); pair round trip; objectMode strings; corked writes; failed batch; errored readable / writable and later readable error destroying the duplex (and what is left untouched); clean `for await` consumption; destroy from `'data'` with a write in flight; `end()` again from `'finish'` |
| `duplex-from.js` | `Duplex.from()` over a lone web stream marks the missing side |
| `gc.js` | forced GCs while only a pending read / `writer.closed` / Duplex.toWeb read+close continuation holds the web side and the node side has pending I/O: all settle |
| `then-pollution.js` | transparent patched `then`: data intact through the three adapters, patch called; hostile `then` during construction: throw, streams unlocked and reusable (Readable, Writable, Duplex `fromWeb`), also when it registers the handlers before throwing (nothing escapes, under the uncaught guard); `Object.prototype.then` getter consulted, data intact |
| `bodies.js` | Response/Request bodies through `Readable.toWeb` (incl. a megabyte); `Readable.fromWeb` over a Response body and a `TextDecoderStream` chain; `Writable.fromWeb` over `IdentityTransformStream` and `FixedLengthStream` (ledger #4); pipeThrough chains in both directions |
| `consumers.js` | `text/json/buffer/arrayBuffer/blob` over web streams; multi-chunk and string decoding; lock release; error propagation; node Readables and async generators |
| `readable-from.js` | `Readable.from(webStream)`: chunk types by objectMode, destroy → cancel + lock release, error propagation |
| `finished-and-abort.js` | ledger #5: hook presence per implementation; `finished()` on readable close/error, writable close/error, settled streams, with a signal; `promises.finished`; `addAbortSignal` on readable/writable, already-aborted, one tee branch (default and byte streams; sibling spared, sibling cancel reaching the source, a teed-away branch inert incl. a pending BYOB read on its branch, cancel settling with a deferred or failing source cleanup in both orders), Response body |
| `compose-web.js` | position validation; single web stream; web head/node tail; web readable into node writable and into web writable; node head/web writable tail; `end()` completing without a consumer; node head/web tail (ledger #5, incl. what the refusal leaves untouched); destroy with a web tail before the first write, under backpressure, behind a closed readable, and bare; a web tail's detached-view chunk failing the composition (writable head under TypeScript; readable-only head on both); a deferred web close() completing cleanly (consumed and readable-only); `Readable.prototype.compose` |
| `pipeline-web.js` | web source/destination/transform stages, generator stages, `TransformStream` head; sink/source/node-sink failures (incl. a node sink failing while the web source is idle, a web sink erroring or rejecting a write while the source is idle, and a web source erroring while a stuck node sink holds the pump); a detached-view chunk failing the pipeline (`TypeError`, source cancelled without a reason); promise-valued chunks by identity; a locked web destination (callback and promise forms, node and web sources); `stream/promises` trailing web destination, `end: false`, signal abort of a node-headed and of an idle all-web pipeline, and during a pending web read |
| `which-impl.js`, `helpers.js` | implementation detection; `once`, `withUncaughtGuard` (fails a test that lets an exception or rejection escape) |

## Legacy (unflagged) behaviors

Guarded by `stream-cpp-legacy.wd-test` (C++ only):

| Behavior | Asserted by |
| --- | --- |
| `Readable.toWeb`, `Writable.toWeb`, `Duplex.toWeb` (writable half first) throw the constructor-gate `Error`, also for destroyed/unreadable inputs | `legacyToWebHitsConstructorGate` |
| `new ReadableStream()` / `new WritableStream()` from `node:stream/web` throw the gate | `legacyStreamWebConstructorsGated` |
| `Readable.fromWeb`, `Writable.fromWeb`, `Duplex.fromWeb` work over runtime-provided streams (fetch bodies, `IdentityTransformStream`) | `legacyFromWebOverRuntimeStreams` |
| `pipeline()` works over runtime-provided web streams | `legacyPipelineOverRuntimeStreams` |
| `compose()` works over runtime-provided web streams | `legacyComposeOverRuntimeStreams` |
| `new TransformStream({ transform })` is an identity transform; the transformer is never called | `legacyTransformStreamIgnoresTransformer` |
