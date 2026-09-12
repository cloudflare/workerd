# node:stream × web streams

An informal specification of the `node:stream` web-interop surface —
`Readable.toWeb/fromWeb`, `Writable.toWeb/fromWeb`, `Duplex.toWeb/fromWeb`,
`Duplex.from`, `Readable.from` over a web stream, `pipeline`, `finished`,
`addAbortSignal`, `node:stream/web`, `node:stream/consumers` —
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
`duplexify`), `streams_pipeline.js`, `streams_end_of_stream.ts`,
`streams_add_abort_signal.ts`, and `src/node/stream/{web,consumers}.js`.

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
  `ERR_STREAM_PREMATURE_CLOSE`.
- `reader.cancel(reason)` destroys the source with `reason` (an
  `AbortError` when none is given) — through `destroyer`, so a `pipeTo()`
  whose destination fails destroys the source with the destination's error.
- A destroyed or ended source, or a Duplex created with `readable: false`,
  yields an already-cancelled stream. A non-Readable is
  `ERR_INVALID_ARG_TYPE`.

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
  decodes to strings.

### Writable.toWeb(writable)

- Accepts anything with `write` and `on` functions; a non-node-stream duck,
  a destroyed or ended Writable, or a Duplex with `writable: false` yields
  an already-closed stream. The strategy follows the writable: its
  `writableHighWaterMark` with the default size of 1 per chunk, or a
  `CountQueuingStrategy` in objectMode.
- Chunks reach the sink as `Writable.prototype.write()` delivers them
  (Buffer identity, `Uint8Array` as a Buffer over the same memory, strings
  as UTF-8, objectMode by identity).
- Backpressure crosses through `'drain'`: a web write whose node `write()`
  returned false stays pending until the node side drains.
- `writer.close()` calls `end()` and settles after `'finish'`; a `_write`
  or `_final` error rejects the pending promises and `writer.closed` with
  the same instance (a synchronous `_write` error also rejects the write
  it belongs to); the node side ending or being destroyed on its own errors
  the stream with an `AbortError`; `writer.abort(reason)` destroys the
  writable with `reason` (an `AbortError` when none).

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
  objectMode passes anything by identity.
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
  writable.

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

### finished / addAbortSignal

- `finished()` and `addAbortSignal()` need the Node.js interop hooks (see
  ledger #5). `addAbortSignal()` on one branch of a tee errors that branch
  alone: the sibling keeps its buffered chunks and its reads, and the
  source is cancelled only once every consumer is gone, with each one's
  reason; on a branch that has itself been teed it does nothing (the
  queued tee model's inert shell, see
  `src/per_isolate/webstreams/AGENTS.md`); a branch's `cancel()` promise
  settles with the source's cleanup in either order.

## Compatibility flags

`stream-cpp.wd-test` pins `streams_enable_constructors` (the toWeb adapters
construct `new ReadableStream()`/`new WritableStream()`, gated under C++)
and `transformstream_enable_standard_constructor` (the transform chains).
`stream-ts.wd-test` omits both; its variants prove the TypeScript
implementation is indifferent to them. `stream-cpp-pedantic.wd-test` runs
the full module set with `pedantic_wpt` added and asserts nothing changes.

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
| 5 | Node.js interop hooks (`Symbol.for('nodejs.webstream.isClosedPromise')`, `…controllerErrorFunction`) | absent; `finished()`, `promises.finished()` and `addAbortSignal()` throw `ERR_WEB_STREAM_INTEROP_UNSUPPORTED` up front | non-enumerable prototype getter and method; the APIs work as in Node, including on native-backed streams (a `Response` body) | `finished-and-abort.js` |

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | `node:stream/web` named and default exports are the globals; adapter statics on the classes and legacy aliases |
| `readable-to-web.js` | delivery; argument validation; byte copy / objectMode identity; derived and explicit strategies with the pause/resume they produce; end, error, premature-close propagation; cancel (with and without reason) and pipeTo-failure destroying the source; unreadable inputs |
| `readable-from-web.js` | delivery; errored/erroring sources through async iteration; validation before locking; lock and locked-input errors (ledger #1); pull on demand; end/close ordering; errors with and without a read in flight; destroy → cancel (reason, `null`, skipped after close); `encoding`, `objectMode`, `highWaterMark`, `signal` |
| `writable-to-web.js` | delivery; close → end → finish; pipeTo completion; sync and async node errors; `_final` error; node-initiated end/destroy → `AbortError`; abort (with and without reason); validation; duck input and unwritable inputs → closed stream (ledger #2); derived strategy; drain-driven backpressure; chunk conversion |
| `writable-from-web.js` | delivery; web error / sink rejection / close rejection destroying the Writable once with no unhandled rejection; back-to-back and corked writes through `_writev`; failed batch; validation before locking; lock (ledger #1); chunk conversion; `decodeStrings`/`objectMode`; end → close; destroy → abort or close; writes complete on sink acceptance |
| `duplex-to-web.js` | pair round trip; validation; destroyed and half Duplexes; non-byte readable (ledger #3); destroy(err) erroring both halves; the whole-Duplex end-of-stream coupling of the halves |
| `duplex-from-web.js` | pair round trip; objectMode strings; corked writes; failed batch; errored readable / writable and later readable error destroying the duplex (and what is left untouched); clean `for await` consumption |
| `duplex-from.js` | `Duplex.from()` over a lone web stream marks the missing side |
| `bodies.js` | Response/Request bodies through `Readable.toWeb` (incl. a megabyte); `Readable.fromWeb` over a Response body and a `TextDecoderStream` chain; `Writable.fromWeb` over `IdentityTransformStream` and `FixedLengthStream` (ledger #4); pipeThrough chains in both directions |
| `consumers.js` | `text/json/buffer/arrayBuffer/blob` over web streams; multi-chunk and string decoding; lock release; error propagation; node Readables and async generators |
| `readable-from.js` | `Readable.from(webStream)`: chunk types by objectMode, destroy → cancel + lock release, error propagation |
| `finished-and-abort.js` | ledger #5: hook presence per implementation; `finished()` on readable close/error, writable close/error, settled streams, with a signal; `promises.finished`; `addAbortSignal` on readable/writable, already-aborted, one tee branch (default and byte streams; sibling spared, sibling cancel reaching the source, a teed-away branch inert incl. a pending BYOB read on its branch, cancel settling with a deferred or failing source cleanup in both orders), Response body |
| `pipeline-web.js` | web source/destination/transform stages, generator stages, `TransformStream` head; sink/source/node-sink failures (incl. a node sink failing while the web source is idle, a web sink erroring or rejecting a write while the source is idle, and a web source erroring while a stuck node sink holds the pump); a detached-view chunk failing the pipeline (`TypeError`, source cancelled without a reason); promise-valued chunks by identity; a locked web destination (callback and promise forms, node and web sources); `stream/promises` trailing web destination, `end: false`, signal abort of a node-headed and of an idle all-web pipeline, and during a pending web read |
| `which-impl.js` | implementation detection |

## Legacy (unflagged) behaviors

Guarded by `stream-cpp-legacy.wd-test` (C++ only):

| Behavior | Asserted by |
| --- | --- |
| `Readable.toWeb`, `Writable.toWeb`, `Duplex.toWeb` (writable half first) throw the constructor-gate `Error`, also for destroyed/unreadable inputs | `legacyToWebHitsConstructorGate` |
| `new ReadableStream()` / `new WritableStream()` from `node:stream/web` throw the gate | `legacyStreamWebConstructorsGated` |
| `Readable.fromWeb`, `Writable.fromWeb`, `Duplex.fromWeb` work over runtime-provided streams (fetch bodies, `IdentityTransformStream`) | `legacyFromWebOverRuntimeStreams` |
| `pipeline()` works over runtime-provided web streams | `legacyPipelineOverRuntimeStreams` |
| `new TransformStream({ transform })` is an identity transform; the transformer is never called | `legacyTransformStreamIgnoresTransformer` |
