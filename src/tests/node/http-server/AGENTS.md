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
(`Server#onRequest`/`#toReqRes`, and `ServerResponse`: the Response
promise, `#toFetchResponse`, `destroy`/`#emitClose`),
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
  destinations and `pipeline(req, TransformStream, res)` work.
- `destroy()`: 'aborted' when the body was not complete, 'error' only when
  the message has an 'error' listener (an unlistened `destroy(err)` is
  swallowed), 'close' always; the body stream is cancelled with the destroy
  reason (`undefined` for a bare `destroy()`), through the held reader when
  the pump had acquired one, unless the body was already read to completion.
  A read pending across `destroy()` is dropped.

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
  dropped, fewer sent as they are). 204 and 304, and the reply to a HEAD
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
- `destroy(err)` emits 'error' then 'close'. Before the headers, the
  Response promise rejects with `err` — or, for a bare `destroy()`, with
  `ERR_STREAM_PREMATURE_CLOSE` (a `TypeError` 'Premature close') — so the
  fetch fails instead of waiting. After the headers, the body errors with
  `err` or that `ERR_STREAM_PREMATURE_CLOSE`; nothing reaches the body's
  controller once it has closed or errored.
- The body stream's `cancel(reason)` — a client abandoning the response —
  destroys the response with `reason`: 'error', 'close', later writes fail
  with `ERR_STREAM_DESTROYED`.

## Compatibility flags

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `streams_enable_constructors` (2022-11-30) | the `ServerResponse` can build its body stream (and the tests their web streams) | `legacyFirstBodyWriteHitsConstructorGate`, `legacyUncaughtGateErrorFailsFetch` |
| `transformstream_enable_standard_constructor` (2022-11-30) | `new TransformStream({ transform })` in the pipeline tests (gate pinned in `src/tests/node/stream`) | — |
| `enable_nodejs_http_modules`, `enable_nodejs_http_server_modules` (2025-08-15 / 2025-09-01) | the server classes; pinned in every cell | — |

`nodejs_compat_v2` is deliberately absent from every cell: the http layer
must not depend on its globals. `http-server-ts.wd-test` omits the
streams-semantic flags; its variants prove the TypeScript implementation is
indifferent to them.

## Divergence ledger (C++ vs TypeScript)

No divergence is observable through the server: every assertion holds
unchanged under both implementations. (One would surface if the response
fed its controller after closing it: with a chunk still queued, the
TypeScript implementation honors a late `controller.error()` per spec while
the C++ one ignores it. The response never does so — see "The response
lifecycle" — and the pure-streams behavior belongs to
`src/tests/streams/readable`.)

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `request-body.js` | GET ends at once; Buffer/string chunks and `complete`; late 'data' listener; 256 KiB in several events; streaming body incremental + chunked headers; `FixedLengthStream` Content-Length; pause/resume (small chunks, and a body above the high-water mark); pipe echo; several pipe destinations; `pipeline` through a `TransformStream` |
| `request-destroy.js` | `destroy(err)` with listener; bare `destroy()` closes quietly; unlistened `destroy(err)` swallowed; mid-body destroy cancels the body stream with the reason and stops 'data'; bare destroy cancels with `undefined`; no cancel after completion |
| `response-body.js` | implicit headers and chunk types; streaming before `end()`; large and many writes; Content-Length capping; 204/304; HEAD (`_hasBody`, dropped writes, null body); `rejectNonStandardBodyWrites`; cork/uncork; backpressure signaling and 'drain' parity; acceptance after headers with `highWaterMark`; web source pipelined in; 'finish' then 'close' with `closed`; write after end via callback only |
| `buffer-lifecycle.js` | fill/write/refill after the callback (intact, not detached, both payloads received); chunk given to `end()` and its parent allocation intact; mutation after the callback not sent; SAB and WebAssembly.Memory views written, 'finish' only; Content-Length-trimmed writes leave their buffers intact; empty and detached views accepted and skipped |
| `response-lifecycle.js` | `destroy(err)` before headers rejects the fetch with it; bare destroy before headers → 'Premature close'; `destroy(err)` after headers errors the body, 'error' then 'close'; bare destroy after headers → premature close, 'close' only; client cancel → destroyed with the reason, `ERR_STREAM_DESTROYED` on later writes |
| `harness.js`, `which-impl.js` | shared machinery |

## Legacy (unflagged) behaviors

Guarded by `http-server-cpp-legacy.wd-test` (C++ only):

| Behavior | Asserted by |
| --- | --- |
| `writeHead()` succeeds; the first body write throws the constructor-gate `Error` synchronously; a handler that catches it and ends anyway never yields a Response — the fetch fails with 'Premature close' | `legacyFirstBodyWriteHitsConstructorGate` |
| The gate `Error` left uncaught fails the fetch with that `Error` | `legacyUncaughtGateErrorFailsFetch` |
| 204, 304 and the reply to a HEAD construct no stream: status and headers delivered, body null, writes dropped | `legacyBodilessResponsesWork` |
| The request body (a runtime stream) is pumped as usual | `legacyRequestBodyIsPumped` |
