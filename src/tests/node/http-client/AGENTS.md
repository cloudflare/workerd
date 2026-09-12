# node:http client × web streams

An informal specification of how a `node:http` `ClientRequest` drives
`fetch()` and the web streams underneath it — the request body it gathers
and hands to `fetch()`, and the `Response` body it pumps into its
`IncomingMessage` — derived from and kept in lockstep with the test suite
in this directory. **The tests are the normative artifact**; this document
maps behaviors to the tests that assert them. Every test runs against the
C++ streams implementation (`http-client-cpp.wd-test`) and the TypeScript
one (`http-client-ts.wd-test`). The general client surface (headers,
options, paths, methods, `Agent`, the Host header, SSRF guards) is owned by
`src/workerd/api/node/tests/http-client-nodejs-test.js` and its siblings;
this suite owns the STREAMS interaction and the exchange's lifecycle only.

The implementation under test is `src/node/internal/internal_http_client.ts`
(`ClientRequest`: `write`/`end`/`#onFinish`, `#handleFetchResponse`,
`#handleFetchError`, `destroy`, `abort`, `setTimeout`/`#armTimer`,
`#emitClose`), `internal_http_incoming.ts` (`#setFetchResponse`,
`#tryRead`, `_destroy`) and `OutgoingMessage` in
`internal_http_outgoing.ts`.

## Infrastructure

A node sidecar (`http-servers.js`) runs one HTTP server, routed by path,
whose port arrives through the `HTTP_SERVER_PORT` binding (plus
`SIDECAR_HOSTNAME`): body echo (`/echo`) and summary (`/sink`), fixed
replies (`/pong`, `/asd`), chunked (`/chunked`) and large (`/large`)
bodies, bodiless statuses (`/status/CODE`), a compressed body (`/gzip`), a
connection dropped mid-body (`/error-mid-body`), a response held open whose
close the sidecar records (`/never-ends?id=` + `/stats?id=`), and delayed
headers or body (`/slow-headers`, `/slow-body`). A second, raw TCP server
(`HTTP_RAW_PORT`) writes hand-crafted replies, segment by segment 20 ms
apart and with `Connection: close` (the runtime would otherwise pipeline
the next request onto the connection): a body short of its Content-Length
(`/short-body`), bytes beyond it (`/long-body`), malformed chunked framing
after a good chunk (`/bad-chunked`), nothing at all (`/empty-reply`), a
garbage status line (`/garbage`). `harness.js` builds the requests
(`request`, `get`, `getRaw`) and collects responses and events. Both cells
need an `internet` network service allowing `private`.

## Core semantics

### The request body

- `write()` and `end()` gather chunks — strings (UTF-8, or the given
  encoding), Buffers, `Uint8Array`s — and `end()` sends them all at once
  as one `Blob`, typed by the Content-Type header: nothing reaches the
  server before `end()`, and the request carries a Content-Length (the
  byte count of everything written) rather than chunked encoding. A body
  without a Content-Type carries none; a POST that ends without writing
  sends Content-Length 0; GET and HEAD send no body whatever is written.
- A chunk's bytes are captured at `write()` (copied), as Node has them on
  the wire by then: mutating, detaching or shrinking the buffer afterwards
  does not change what is sent. Views over a `SharedArrayBuffer` or a
  `WebAssembly.Memory` are sent like any other; a zero-length view, a
  detached one included, is accepted and sends nothing.
- `end()` ends the body at once (`writableEnded`). A `write()` afterwards
  — one issued from inside 'finish' included — is a write after end, as in
  Node: it returns false, reports `ERR_STREAM_WRITE_AFTER_END` to its
  callback and as the request's 'error', and sends nothing; so does an
  `end(chunk)` after `end()`. The request, already on its way, completes
  with what was written before. A bare `end(cb)` after `end()` calls back
  at 'finish', or at once with `ERR_STREAM_ALREADY_FINISHED` once the
  request has finished (the exchange over and the request destroyed
  included).
- The request is sent through `fetch()` with `redirect: 'manual'` and
  `encodeResponseBody: 'manual'`; a finished request stays live until its
  exchange ends.

### The response body

- The `Response` body is pumped into the `IncomingMessage` by one default
  reader, acquired on the first `_read()` and held for the message's
  lifetime, until backpressure or EOF. Chunks arrive as Buffers (strings
  under `setEncoding`), incrementally as the server writes them, whole for
  large bodies; `pause()`/`resume()` hold and continue delivery without
  loss. Bodiless statuses (204, 304, an empty 200) and the reply to a HEAD
  end at once with `complete` set. Without a consumer the body waits —
  unless nobody could consume it: a response arriving with no 'response'
  listener is dumped, as Node's is (its body consumed and dropped, the
  response completing and the request closing), so `finished(req)`
  resolves and nothing holds the exchange open; `req.res` is set all the
  same. `pipe()` is the Readable's: a slow destination's backpressure pauses
  the response and 'drain' resumes it.
- Bytes pass through untouched: a compressed body keeps its
  Content-Encoding and is not decompressed.

### The exchange's lifecycle

- A completed exchange: `req.res` is the response; the response ends,
  the request closes (destroyed, as Node leaves it), the response closes —
  once.
- `res.destroy(err)`: 'aborted' (the body was not complete), the error on
  the response — and, as a socket error would in Node, on the request —
  then 'close'. The body stream is cancelled, which the server observes as
  its connection closing. The server dropping the connection mid-body
  aborts the response the same way, with the runtime's error on both; so
  do a body cut short of its Content-Length and malformed chunked framing
  (the bytes before the fault are delivered, `complete` stays false).
  Bytes beyond the Content-Length are not the body: the response ends,
  complete, after the announced length. A reply the runtime cannot parse
  at all — the connection closing without a byte, a garbage status line —
  never becomes a response: the request errors (a codeless `Error` with
  the runtime's text: "Network connection lost." for the former, an
  "internal error; reference = …" for the latter and for the framing
  fault — not pinned) and closes.
- `req.destroy(err)`: the request is destroyed at once and closes on a
  later tick; it reports `err`, or `Error: socket hang up` (ECONNRESET)
  for a bare `destroy()` before any response, on the next tick. A pending
  fetch is aborted — a response arriving afterwards is dropped and its
  body cancelled — and `end()` after `destroy()` sends nothing (no
  'finish'). A response in flight is aborted at once with `err` or
  ECONNRESET 'aborted': 'aborted', then (request 'error',) request 'close',
  response 'error' (for listeners), response 'close'; the server sees the
  connection close.
- A fetch that fails before yielding a response (a connection that cannot
  be made) destroys the request with the failure: 'error', 'close'. So
  does the send itself failing synchronously — a `host` or `path` mutated
  into something no URL can hold, a body the Blob cannot take, `fetch()`
  throwing, a `Promise.prototype.then` that throws on its promise — rather
  than the throw escaping the 'finish' emission and the request never
  being sent.
- `req.abort()`: `aborted` and `destroyed` are set at once, 'abort' fires
  on the next tick, and the request is destroyed without the 'socket hang
  up': 'abort', 'close' before any response; 'aborted', 'abort', 'close',
  response 'error' (ECONNRESET 'aborted'), response 'close' mid-body. A
  second `abort()` is inert.
- `setTimeout(ms)` (or the `timeout` option) arms one idle timer, running
  from the moment the request has been sent and the timeout set, whichever
  is later, and started over by the response's arrival and by every chunk
  the body pump reads — as Node's socket timer is by every byte — so a body
  that keeps arriving never times out, only `ms` of silence does;
  `setTimeout(0)` clears it and a completed exchange disarms it. Firing
  emits 'timeout' on the request and its response, once, then destroys the
  request with an `AbortError` (code `ABORT_ERR`) — where Node only emits
  and leaves the teardown to the listener. The response's
  `setTimeout(ms, cb)` sets the same timer (in Node, both set the socket's
  idle timer; the last call wins, `0` clears) and registers `cb` as its
  'timeout' listener. Either side validates `ms` as Node's
  `socket.setTimeout` does: `ERR_INVALID_ARG_TYPE` for a non-number,
  `ERR_OUT_OF_RANGE` for a negative or non-finite one.
- The `signal` option is armed for the whole exchange: aborted before the
  request is sent (already aborted, or before `end()`), the request errors
  with an `AbortError` (`ABORT_ERR`) and closes, nothing sent; aborted
  while the response arrives, the response aborts as on a timeout, both
  reporting an `AbortError` whose `cause` is the signal's reason; aborted
  after the exchange, nothing happens.
- `stream.finished(req)` reports the request's 'close' — the end of the
  exchange — not its 'finish' (the request sent), as in Node, where the
  request is a legacy stream that `finished()` waits on as a readable too;
  `end-of-stream` gives a workerd `OutgoingMessage` (a `Writable`) the same
  treatment by shape.

### Re-entrancy

- `req.destroy()` from inside 'response' aborts the response just handed
  over (no 'socket hang up': the response exists). `req.abort()` from
  inside 'timeout' is the teardown that counts — the response is aborted at
  once, then hears its 'timeout', and the timer's own `destroy(AbortError)`
  finds the request destroyed: ECONNRESET on the response, no `AbortError`.
  `req.setTimeout()` from inside 'timeout' re-arms nothing that survives
  the teardown that follows the listeners. `res.destroy(err)` from inside
  'aborted' changes nothing: the response is already being destroyed with
  ECONNRESET.

### Prototype pollution

- The client uses no primordials: a patched `Promise.prototype.then` sees
  the exchange's hops (the count is not pinned) and a transparent patch
  changes nothing; a `then` that throws as the request is sent fails the
  request ('error', 'close', no 'response').

### Interop

- The response is a node Readable: `res.pipe(Writable.fromWeb(ws))`,
  `Readable.toWeb(res)` as a `Response` body, `pipeline(res,
  TransformStream, sink)`, `stream/consumers` and `for await` all drain it,
  leaving it complete.

## Compatibility flags

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `enable_nodejs_http_modules` (2025-08-15) | the client classes; pinned in every cell | — |
| `nodejs_zlib` (2024-09-23) | `node:zlib` for the compression-passthrough check | — |
| `streams_enable_constructors`, `transformstream_enable_standard_constructor` (2022-11-30) | the interop tests' web streams (gate pinned in `src/tests/node/stream`) | — |

The client constructs no web streams of its own (the request body is a
`Blob`, the response body the runtime's), so there is no legacy cell.
`nodejs_compat_v2` is deliberately absent from every cell.
`http-client-ts.wd-test` omits the streams-semantic flags; its variants
prove the TypeScript implementation is indifferent to them.

### Accepted compatibility risk: the unflagged 'socket hang up'

Historically, a bare `req.destroy()` of a request that had no response yet
was silent: the request neither errored nor closed (and, having been
marked destroyed by `end()`, often ignored the `destroy()` altogether). It
now reports `Error: socket hang up` (ECONNRESET) on the next tick, as
Node's does (see "The exchange's lifecycle"), without a compatibility
flag. A Worker that destroys an unsent request and has no `'error'`
listener therefore gets an uncaught exception where it previously got
nothing. The risk was accepted deliberately: the pattern is rare in
practice (a request destroyed before it is even sent), the previous
behavior was itself a defect rather than a contract, and the new one is
what Node documents and what code written against Node already handles.
`req.abort()` remains the quiet teardown.

The same reasoning applies to a `write()` or `end(chunk)` after `end()`:
historically accepted (`true`, callback called) and silently dropped — and
a late `end(chunk)` even kept the request from ever being sent — it now
reports `ERR_STREAM_WRITE_AFTER_END` through the callback and as 'error',
as Node's does.

## Divergence ledger (C++ vs TypeScript)

No divergence is observable through the client: every assertion holds
unchanged under both implementations.

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `response-body.js` | Buffer chunks and `complete`; `setEncoding`; incremental chunked delivery; a megabyte intact; pause/resume; bodiless statuses; HEAD; compression passthrough; waiting for a consumer |
| `request-body.js` | string body echoed with the server-side Content-Type/Length; `end(Buffer)` sent once; chunk forms and encodings; nothing sent before `end()`; length and type as the server sees them; empty POST; chunk captured at `write()` (mutated, detached, shrunk afterwards); SAB/WebAssembly.Memory views sent, empty and detached views accepted; GET/HEAD ignore writes; `write()`/`end(chunk)` after `end()` (from 'finish' too) → `ERR_STREAM_WRITE_AFTER_END`, request still completes; bare `end(cb)` after `end()` and after the exchange; non-byte chunks → `ERR_INVALID_ARG_TYPE` synchronously |
| `reentrancy.js` | destroy from 'response'; abort and `setTimeout()` from 'timeout'; `res.destroy()` from 'aborted' |
| `lifecycle.js` | `res.destroy()` closes; end then one close; `res.destroy(err)` mid-body reaching the server; server dropping the connection; completed response final; connection failure; truncated body (raw server) aborting the response; bytes beyond Content-Length ignored; malformed chunked framing aborting after the good chunk; empty and garbage replies failing the request with no 'response'; `req.res` and request close after the response; `req.destroy()` before the response (hang up), with an error, before `end()`, mid-body (bare and with an error), response after destroy dropped; `abort()` before the response, before `end()`, mid-body; timeout before headers (armed before/after `end()`), `timeout` option and callback, mid-body, idle not deadline (a 600 ms trickle passing a 250 ms timeout), disarmed by completion, cleared by `setTimeout(0)`; the response's `setTimeout` arming the timer (callback, teardown shape), replacing the request's, clearing with `0`; `ms` validation on both sides; `signal` already aborted / aborted before `end()` / mid-body (with cause) / after completion; `finished(req)` after 'close'; a synchronous send failure (invalid host) → 'error', 'close'; a response with no 'response' listener dumped (request closes, `finished(req)` resolves, `res.complete`) |
| `then-pollution.js` | transparent patched `then` (echo intact); hostile `then` during the send → request 'error', 'close', no 'response' |
| `interop.js` | pipe into `Writable.fromWeb`; pipe into a 16 KiB slow sink (bounded buffer, pauses); `Readable.toWeb` body; pipeline through a `TransformStream`; `stream/consumers` and async iteration |
| `harness.js`, `which-impl.js` | shared machinery |
