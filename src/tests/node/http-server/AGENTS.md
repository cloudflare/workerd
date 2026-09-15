# node:http server × web streams

An informal specification of how a `node:http` `Server` drives the web
streams underneath it — the `Request` body it pumps into the
`IncomingMessage`, and the `ReadableStream` the `ServerResponse` builds as
the body of the `Response` it hands back to `fetch()` — derived from and
kept in lockstep with the test suite in this directory. **The tests are
the normative artifact**; this document maps behaviors to the tests that
assert them. Every test runs against the C++ streams implementation
(`http-server-cpp.wd-test`) and the TypeScript one
(`http-server-ts.wd-test`). The general server surface (headers, options,
ports, `listen`/`close` lifecycle, `cloudflare:node` helpers) is owned by
`src/workerd/api/node/tests/http-server-nodejs-test.js`; this suite owns
the STREAMS interaction only.

The implementation under test is `src/node/internal/internal_http_server.ts`
(`Server#onRequest`/`#toReqRes` and its `[captureRejectionSymbol]`, and
`ServerResponse`: the Response promise, `#toFetchResponse`,
`destroy`/`#emitClose`),
`internal_http_incoming.ts` (`IncomingMessage#tryRead`, `_read`,
`_destroy`) and the `OutgoingMessage` write path in
`internal_http_outgoing.ts`.

## Infrastructure

No sidecar. The worker is bound to itself as `SERVICE`; its default
handler (`main.js`) routes every incoming Request to the current test's
server through `handleAsNodeRequest`. `harness.js` offers the two ways a
Request reaches a server:

- `env.SERVICE.fetch(...)`: through the service binding. The runtime pumps
  bodies across it (the production shape); a cancellation on one side
  reaches the other only once the exchange completes.
- `dispatch(new Request(...))`: an in-isolate Request handed to the server
  directly, so its body stream IS the test's stream and cancellation is
  observable at once. Tests that need it call `remember(env, ctrl)` first.

Tests run sequentially, one server (`withServer`) at a time.

## Core semantics

### The request body

- The `Request` body is pumped into the `IncomingMessage` by one default
  reader, acquired on the first `_read()` and held for the message's
  lifetime; the pump reads until `push()` reports backpressure or EOF and
  `_read()` restarts it with the same reader. A request without a body
  (GET) ends at once with `complete` set.
- Chunks arrive as Buffers (strings under `setEncoding`), whole and in
  order; a body the client streams arrives incrementally and is chunked
  (no Content-Length), a `FixedLengthStream` body announces its length. A
  'data' listener attached inside the handler still sees the body.
- `pause()` holds delivery, `resume()` continues it without loss, also for
  a body larger than the high-water mark. `pipe()` to one or several node
  destinations and `pipeline(req, TransformStream, res)` work; `pipe()` is
  the Readable's: a destination's backpressure pauses the body and 'drain'
  resumes it, the destination hears 'pipe'/'unpipe', a destination that
  errors is unpiped (no further write reaches it), `unpipe()` stops
  delivery and pauses a source left without destinations, and a source
  error is not forwarded to the destination (that is `pipeline()`'s job).
- `destroy()`: 'aborted' when the body was not complete, 'error' only when
  the message has an 'error' listener (an unlistened `destroy(err)` is
  swallowed), 'close' always; the body stream is cancelled with the destroy
  reason (`undefined` for a bare `destroy()`), through the held reader when
  the pump had acquired one, unless the body was already read to completion.
  A read pending across `destroy()` is dropped, however the stream settles
  it — also on the runtime's stream across the binding (ledger #1).
- The body stream failing under the message — erroring mid-upload, or while
  the handler has the message paused with a read pending underneath, or
  yielding a chunk the message cannot take (a view over a detached
  ArrayBuffer: the conversion's `TypeError`) — aborts it: 'aborted', the
  error, 'close' with `complete` false; the response can still be sent.
  `pause()` then `resume()` inside every 'data' loses nothing.

### The response body

- Headers go out at the first `write()`/`end()` (`writeHead()` only formats
  them; the first write sends them implicitly if needed), which resolves
  the `Response` — while the handler is still writing. The body is a
  `new ReadableStream({ type: 'bytes' })`: writes before the headers are
  buffered and flushed into it at that point, later writes are enqueued
  as they come, so a client reads chunks before `end()`.
- Every chunk type is delivered (string, Buffer, `Uint8Array`, explicit
  encoding), empty writes contribute nothing, many small and large writes
  arrive whole. A declared Content-Length caps the body (extra bytes
  dropped, fewer sent as they are) at `parseInt`'s reading of it — a
  non-numeric value leaves the body uncapped, zero or a negative value
  drops every chunk, a fraction or padded number caps at its integer part.
  (The header value itself is not validated; whether a malformed one keeps
  reaching the client is deliberately unpinned.) 204 and 304, and the reply to a HEAD
  (marked bodiless before the handler runs), have a null body and drop
  their writes — accepted, callback called — or, under the server's
  `rejectNonStandardBodyWrites` option, refuse them with
  `ERR_HTTP_BODY_NOT_ALLOWED`. A web `ReadableStream` can be `pipeline()`d
  into the response.
- A written buffer stays the caller's: the response copies each chunk as
  it flushes it into the body stream (whose enqueue would otherwise
  transfer, i.e. detach, the buffer), so a buffer is reusable once the
  write's callback has fired and a mutation after that is not sent (one in
  the same tick as the write is, as with Node's corked socket). Views over
  a `SharedArrayBuffer` or a `WebAssembly.Memory` are written like any
  other; zero-length views, detached ones included, contribute nothing.
- `write()` reports backpressure against the response's own buffer before
  the headers, with 'drain' following; once the headers are out every
  write is accepted (the body stream queues whatever the handler writes;
  the client's consumption does not feed back), so no 'drain' is owed.
  `cork()`/`uncork()` batch writes (`writableLength` counts the header
  bytes too). The server's `highWaterMark` option is the response's
  `writableHighWaterMark`.

### The response lifecycle

- 'finish' fires once the body has been handed off, and closes the stream;
  'close' follows it (a microtask later), once, and marks the response
  destroyed: a `write()` after `end()` fails through its callback with
  `ERR_STREAM_WRITE_AFTER_END` (a second `end()` is inert) and, once the
  response is closed, never as an 'error' event.
- Once `end()` has been called, `destroy()` — with or without an error,
  from inside 'finish' or right after `end()` — aborts nothing: the body
  was handed off by `end()`, so the whole of it reaches the client, no
  'error' fires and 'close' still follows 'finish', once. The response is
  marked `destroyed` (and `errored` with the reason) at once, as Node's
  `OutgoingMessage.destroy(error)` marks it, so a write that follows fails
  through its callback alone. Nothing escapes the isolate. (`destroy()`
  takes the error alone, as Node's does; there is no callback.)
- Before that, `destroy(err)` emits 'error' then 'close'. Before the
  headers, the Response promise rejects with `err` — or, for a bare
  `destroy()`, with `ERR_STREAM_PREMATURE_CLOSE` (a `TypeError` 'Premature
  close') — so the fetch fails instead of waiting. After the headers, the
  body errors with `err` or that `ERR_STREAM_PREMATURE_CLOSE`; nothing
  reaches the body's controller once it has closed or errored — a chunk the
  message buffer still holds when the response is destroyed is dropped, as
  a destroyed socket's pending writes are in Node.
- A request listener that throws synchronously destroys the response with
  the error ('error', 'close') and the fetch rejects with it — also after
  `writeHead()` and a `write()`, whose Response is then discarded. An
  async listener whose promise rejects ends the same way (the server
  captures its listeners' rejections; Node's process would die of the
  unhandled rejection, a Worker cannot): before any header the fetch
  rejects with the error, after a partial body the Response's body errors
  with it.
- Another event's listener rejecting (a 'listening' listener, say) is
  reported as `EventEmitter`'s own capture fallback reports one: as the
  server's 'error' event — heard by an 'error' listener, thrown uncaught
  without one. An 'error' listener's own rejection is re-raised uncaught,
  once, never emitted as another 'error'.
- The body stream's `cancel(reason)` — a client abandoning the response —
  destroys the response with `reason`: 'error', 'close', later writes fail
  with `ERR_STREAM_DESTROYED`.

## Compatibility flags

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `streams_enable_constructors` (2022-11-30) | the `ServerResponse` can build its body stream (and the tests their web streams) | `legacyFirstBodyWriteHitsConstructorGate`, `legacyUncaughtGateErrorFailsFetch` |
| `transformstream_enable_standard_constructor` (2022-11-30) | `new TransformStream({ transform })` in the pipeline tests (gate pinned in `src/tests/node/stream`) | — |
| `enable_nodejs_http_modules`, `enable_nodejs_http_server_modules` (2025-08-15 / 2025-09-01) | the server classes; pinned in every cell | — |
| `unhandled_rejection_after_microtask_checkpoint` (2026-03-03) | accurate `unhandledrejection` reporting, which `collectUncaught` relies on: the C++ implementation settles a read pending at `cancel()` through a rejected promise adopted a tick later, which the earlier tracker reported before its handler ran (ledger #1) | `legacyPendingReadCancelMisfiresAsUnhandledRejection` |

`nodejs_compat_v2` is deliberately absent from every cell: the http layer
must not depend on its globals. `http-server-ts.wd-test` omits the three
flags the TypeScript implementation does not consult —
`streams_enable_constructors`, `transformstream_enable_standard_constructor`
and `unhandled_rejection_after_microtask_checkpoint` — and its variants
prove the implementation is indifferent to them (its cancel resolves a
pending read `done`, so no rejected promise exists for the earlier tracker
to misreport).

## Divergence ledger (C++ vs TypeScript)

Every assertion holds unchanged under both implementations; the one
divergence underneath is swallowed by the message and is observable only
through the isolate's rejection bookkeeping.

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | The body pump's read pending when `destroy()` cancels the runtime's body stream (across the binding) | the read rejects with the cancel reason ("Stream was cancelled." for a bare `destroy()`); `#tryRead` swallows it (the message is already destroyed). Without `unhandled_rejection_after_microtask_checkpoint` the tracker reports the rejected promise before that handler runs: a spurious `unhandledrejection` | the read resolves `done` (spec); nothing to report at any compat date | `destroyWithPendingReadAcrossBinding` (identical events, nothing escapes, under both — the cpp cell pins the flag), `legacyPendingReadCancelMisfiresAsUnhandledRejection` (the misfire, unflagged C++); the pure-streams behavior is `src/tests/streams/identity` ledger #20 |

(A second divergence would surface if the response fed its controller after
closing it: with a chunk still queued, the TypeScript implementation honors
a late `controller.error()` per spec while the C++ one ignores it. The
response never does so — see "The response lifecycle" — and the
pure-streams behavior belongs to `src/tests/streams/readable`.)

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `request-body.js` | GET ends at once; Buffer/string chunks and `complete`; late 'data' listener; 256 KiB in several events; streaming body incremental + chunked headers; `FixedLengthStream` Content-Length; pause/resume (small chunks, and a body above the high-water mark); pipe echo; several pipe destinations; `pipeline` through a `TransformStream` |
| `request-destroy.js` | `destroy(err)` with listener; bare `destroy()` closes quietly; unlistened `destroy(err)` swallowed; mid-body destroy cancels the body stream with the reason and stops 'data'; bare destroy cancels with `undefined`; destroy with the pump's read pending on the runtime's stream across the binding (aborted, closed incomplete, response sent, nothing escapes — ledger #1); no cancel after completion |
| `response-body.js` | implicit headers and chunk types; streaming before `end()`; large and many writes; Content-Length capping; 204/304; HEAD (`_hasBody`, dropped writes, null body); `rejectNonStandardBodyWrites`; cork/uncork; backpressure signaling and 'drain' parity; acceptance after headers with `highWaterMark`; web source pipelined in; 'finish' then 'close' with `closed`; write after end via callback only; Content-Length lies cap the body at `parseInt`'s reading (`abc` uncapped, `0` and negative empty, fraction and padded at the integer part; the header echo deliberately unasserted) |
| `piping.js` | 1 MiB into a 16 KiB slow sink: bounded buffer, pauses/resumes, all bytes; `unpipe()` after the first chunk ('pipe'/'unpipe' on the destination, delivery stops, source paused, rest to a 'data' listener, destination not ended); erroring destination unpiped ('unpipe' before its 'error', one write only, source paused); source error not forwarded (destination stays piped, open, unerrored) |
| `request-body-failures.js` | body stream erroring mid-upload and while paused (aborted, error, close incomplete, response still sent); a detached-view chunk (`TypeError`) |
| `reentrancy.js` | `destroy()` and `destroy(err)` inside 'finish' (body whole, 'close' once, no 'error', nothing escapes); pause/resume inside every 'data' |
| `then-pollution.js` | transparent patched `then`: request and response bodies intact |
| `data-volumes.js` | an 8 MiB request body across the binding (whole, in order, several chunks); 20,000 one-byte response writes; alternating string/Buffer/Uint8Array/empty writes; a body with UTF-8 sequences split byte by byte, reassembled by `setEncoding('utf8')` |
| `buffer-lifecycle.js` | fill/write/refill after the callback (intact, not detached, both payloads received); chunk given to `end()` and its parent allocation intact; mutation after the callback not sent; SAB and WebAssembly.Memory views written, 'finish' only; Content-Length-trimmed writes leave their buffers intact; empty and detached views accepted and skipped |
| `response-lifecycle.js` | `destroy(err)` before headers rejects the fetch with it; bare destroy before headers → 'Premature close'; `destroy(err)` after headers errors the body, 'error' then 'close'; bare destroy after headers → premature close, 'close' only; `destroy(err)` after `end()` aborts nothing (chunk in `end()`, bare `end()` after a write, nothing written): marked destroyed and errored at once, a later write fails through its callback, body whole, 'finish' then 'close', no 'error', nothing escapes; client cancel → destroyed with the reason, `ERR_STREAM_DESTROYED` on later writes; a listener throwing before headers / after a partial body → fetch rejects with it, nothing else escapes; an async listener rejecting before headers / after a partial body → destroyed with the error, fetch or body failing with it |
| `listener-rejections.js` | an async 'listening' listener rejecting → the server's 'error' event (heard by a listener; thrown uncaught without one); an async 'error' listener's own rejection → uncaught once, not re-emitted |
| `harness.js`, `which-impl.js` | shared machinery (`collectUncaught` gathers what escapes the isolate during a test) |

## Legacy (unflagged) behaviors

Guarded by `http-server-cpp-legacy.wd-test` (C++ only):

| Behavior | Asserted by |
| --- | --- |
| `writeHead()` succeeds; the first body write throws the constructor-gate `Error` synchronously; a handler that catches it and ends anyway never yields a Response — the fetch fails with 'Premature close' | `legacyFirstBodyWriteHitsConstructorGate` |
| The gate `Error` left uncaught fails the fetch with that `Error` | `legacyUncaughtGateErrorFailsFetch` |
| 204, 304 and the reply to a HEAD construct no stream: status and headers delivered, body null, writes dropped | `legacyBodilessResponsesWork` |
| The request body (a runtime stream) is pumped as usual | `legacyRequestBodyIsPumped` |
| Without `unhandled_rejection_after_microtask_checkpoint`, `req.destroy()` with the pump's read pending on the runtime's stream surfaces a spurious `unhandledrejection` ('Stream was cancelled.') — the message still aborts and the response is sent (ledger #1) | `legacyPendingReadCancelMisfiresAsUnhandledRejection` |
