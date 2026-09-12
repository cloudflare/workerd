# node:net × web streams

An informal specification of how a `node:net` `Socket` drives the two
web-stream halves of the `connect()` socket underneath it, derived from
and kept in lockstep with the test suite in this directory. **The tests
are the normative artifact**; this document maps behaviors to the tests
that assert them. Every test runs against the C++ streams implementation
(`net-cpp.wd-test`) and the TypeScript one (`net-ts.wd-test`). The general
`net` surface (option validation, DNS, `BlockList`, `SocketAddress`,
`BoundSocket`, abort signals, reconnect) is owned by
`src/workerd/api/node/tests/net-nodejs-test.js`; this suite owns the
STREAMS interaction only.

The implementation under test is `src/node/internal/internal_net.ts`:
`initializeConnection` (the handle: a BYOB reader and a default writer over
the `connect()` socket's halves), `startRead` (the read loop),
`_writeGeneric`/`_final`/`_destroy` (the write path), and the half-open,
timeout and byte-accounting logic around them.

## Infrastructure

A node sidecar (`tcp-servers.js`) runs five TCP servers whose ports arrive
through `fromEnvironment` bindings (plus `SIDECAR_HOSTNAME`):

- **echo** (`NET_ECHO_PORT`): echoes every byte; ends after the client ends.
- **end** (`NET_END_PORT`): ends its write side on connect, keeps reading.
- **greet** (`NET_GREET_PORT`): writes one greeting and ends its write side,
  keeps reading.
- **sink** (`NET_SINK_PORT`): counts bytes; on the client's end replies with
  the decimal count and ends.
- **ticker** (`NET_TICKER_PORT`): writes `tick` every 20 ms until the client
  ends.

Both cells need `experimental` (the `connect()` socket) and an `internet`
network service allowing `private`.

## Core semantics

### The handle

- The `connect()` socket is opened with `secureTransport: 'starttls'` and
  **`allowHalfOpen: true`**: the Duplex owns the half-open policy (see
  below), so the native socket must never close the writable half on EOF
  itself. The handle holds `readable.getReader({ mode: 'byob' })` and
  `writable.getWriter()`; both halves stay locked for the socket's life.
- 'connect' then 'ready' fire when the socket opens; `pending`,
  `connecting` and `readyState` track the transition. Writes issued while
  connecting are held and flushed after 'connect', in order with writes
  issued from the 'connect' handler.

### The read loop

- Exactly one loop runs per handle. Each iteration hands a view to a BYOB
  read: a fresh 4 KiB `Uint8Array` by default, or the `onread` option's
  buffer/generator result. Fills are pushed as copied Buffers (or decoded
  by `setEncoding`), or handed to the `onread` callback as a Buffer over
  the current backing store. A callback returning `false`, a `push()`
  returning `false`, or `pause()` stops the loop; `resume()`/`read()`
  restart it.
- The BYOB read transfers the view's buffer. A fixed `onread` buffer is
  therefore continued over the transferred backing store as a view of the
  same range — the caller's offset and capacity, so a view into a larger
  allocation never grows to it and the bytes around it are never written;
  the caller's original `Uint8Array` is detached from the first read on,
  and each callback's Buffer is valid until the next read.
- EOF (`done`) pushes EOF and reads zero bytes, so 'end' fires at once even
  with no consumer. `bytesRead` counts pushed bytes.
- The loop's failures are the socket's 'error' (then 'close'), never a
  silent stop with the socket open: a generator that throws (its error), a
  generator returning anything but a `Uint8Array` (`ERR_INVALID_ARG_TYPE`;
  Node reuses its previous buffer, which the transferring read has no way
  to), an empty view — a zero-length one, or the fixed buffer once the
  callback has detached it — (`ENOBUFS`, code and `syscall: 'read'`, as
  Node's read into an empty buffer), a view the read refuses (one over a
  `SharedArrayBuffer`: the runtime's `TypeError`). A read rejected because
  the socket is being destroyed, or because a TLS upgrade replaced the
  handle, is ignored.

### The write path

- `_write` encodes strings itself (`decodeStrings` is off) and writes one
  chunk through the writer; `_writev` concatenates a corked batch into one
  write. The write callback fires when the writer's promise settles;
  `bytesWritten` counts the encoded bytes once flushed (queued bytes are
  added from `writableLength`), and `bufferSize` mirrors `writableLength`.
- `end()` → `_final` → `writer.close()` → 'finish'. Backpressure follows the
  Duplex's `highWaterMark`: `write()` returns false and 'drain' follows the
  flush.

### Half-open, end and destroy

- `allowHalfOpen` defaults to false and the 'end' enforcer is registered
  unconditionally. With it false, the peer's EOF → 'end' (socket still not
  destroyed, still writable) → `end()` → 'finish' → auto-destroy → 'close';
  a later `write()` fails with `EPIPE` (and, the socket already being
  destroyed, no 'error' event). With it true the writable side stays open
  after 'end' and writes complete; the client's `end()` closes it.
- `destroy()` closes the `connect()` socket and emits 'close' with
  `hadError`; `destroy(err)` emits 'error' once, then 'close' `true`. A
  write after destroy fails with `ERR_STREAM_DESTROYED`; a write on a socket
  whose handle is gone fails with `ERR_SOCKET_CLOSED`; a non-byte chunk
  throws `ERR_INVALID_ARG_TYPE`. Property access on a closed socket is
  harmless.

### Timeouts

- `setTimeout(ms)` arms a one-shot idle timer; delivered or flushed data
  re-arms it without registering listeners; it fires 'timeout' without
  closing the socket; `setTimeout(0)` clears it, and activity afterwards
  arms nothing.

### Interop

- The socket is a Duplex, so `socket.pipe(Writable.fromWeb(ws))`,
  `Readable.toWeb(socket)` as a Response body (EOF requires the writable
  side finished too — end first), `pipeline(socket, TransformStream, sink)`,
  `pipeline(ReadableStream, socket)` and `Duplex.toWeb(socket)` all work;
  the underlying halves themselves refuse a second consumer (locked).

## Compatibility flags

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `streams_byob_reader_detaches_buffer` (2021-11-10) | the BYOB read transfers the `onread` buffer; the caller's `Uint8Array` is detached | `legacyFixedBufferIsFilledInPlace` |
| `internal_stream_byob_return_view` (2024-05-13) | a BYOB read at EOF returns a zero-length view (the loop checks `done` first; unobservable here) | — |
| `streams_enable_constructors`, `transformstream_enable_standard_constructor` | the interop tests' web streams (gate pinned in `src/tests/node/stream`) | — |

`net-ts.wd-test` omits the streams-semantic flags; its variants prove the
TypeScript implementation is indifferent to them.

## Divergence ledger (C++ vs TypeScript)

No divergence is observable through the socket: every assertion holds
unchanged under both implementations. (The one that existed — the
TypeScript writable rejecting a second `close()` after the native socket
had already closed the half on EOF — was resolved by leaving the half-open
policy to the Duplex; see "The handle".)

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `connect-lifecycle.js` | handle locks (BYOB reader, default writer); connect → ready and state properties; deferred writes before connect (order, callback state, `bytesWritten`); destroy before connect (`ERR_SOCKET_CLOSED_BEFORE_CONNECTION`, no 'connect') |
| `echo-roundtrip.js` | Buffers and event order; `setEncoding` (latin1 byte round trip); 40 KiB multi-byte utf8; byte accounting; 10 MB `bytesWritten`; 256 KiB patterned volume; corked batch |
| `half-close.js` | enforcer registration and default; peer EOF ending both sides; `EPIPE` after EOF; half-open writes after EOF; explicit end with half-open; EOF surfacing without a consumer |
| `end-and-destroy.js` | end callback forms; `bufferSize`; destroy with/without error (events, `hadError`); writes after destroy, without handle, with invalid chunks; inert closed socket; all queued writes flushed before end |
| `backpressure.js` | pause/resume against a ticking peer; paused-mode `read()` restarting the loop; `write()` false and 'drain'; cork cycles |
| `timeouts.js` | idle timeout without closing; data resets; `setTimeout(0)` clears, also across later traffic |
| `onread.js` | fixed buffer across several fills (and its detachment); a fixed view into a larger allocation keeping its range; generated buffers; callback `false` stopping and `resume()` restarting; a throwing generator (its error), garbage from the generator (`ERR_INVALID_ARG_TYPE`), an empty view and a callback-detached fixed buffer (`ENOBUFS`), a SAB view (`TypeError`) — each destroying the socket |
| `interop.js` | pipe into `Writable.fromWeb`; `Readable.toWeb(socket)` body; pipeline through a TransformStream and from a web source; `Duplex.toWeb` round trip; locked halves |
| `servers.js`, `which-impl.js` | shared machinery |

## Legacy (unflagged) behaviors

Guarded by `net-cpp-legacy.wd-test` (C++ only):

| Behavior | Asserted by |
| --- | --- |
| A fixed `onread` buffer is filled in place: never detached, same backing store in every callback, last fill readable from the caller's buffer | `legacyFixedBufferIsFilledInPlace` |
| A fixed `onread` view into a larger allocation is filled within its own range; the bytes around it survive every fill | `legacySubarrayIsFilledWithinItsRange` |
