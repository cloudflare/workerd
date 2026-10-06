# connect() sockets × streams

The readable/writable stream halves of `connect()` TCP sockets under
both stream implementations. **The tests are the normative artifact.**
The general Socket surface (startTls, secureTransport, DNS overrides,
the connect-handler protocol, HTTP-over-socket) is owned by
`src/workerd/api/tests/` (http-socket-test, connect-handler-test,
starttls-*); this suite owns the STREAMS interaction only.

## Infrastructure

A node sidecar (echo-server.js) runs three TCP servers, their ports
delivered through `fromEnvironment` bindings (`STREAMS_ECHO_PORT`,
`STREAMS_GREET_PORT`, `STREAMS_STALL_PORT`, plus `SIDECAR_HOSTNAME`):

- **echo**: echoes every byte; on client half-close, flushes and ends
  (the client's readable reaches EOF after the full echo).
- **greet**: writes one fixed message and ends immediately.
- **stall**: accepts and never reads, so a large write stays in flight.

`startTls()` detaches a socket's streams. The suite's network has no TLS
starter, so the upgraded socket then fails to open (logged as an
uncaught rejection); the detach itself has already happened.

Both cells need `experimental` (Socket) and an `internet` network
service allowing `private`. The suite's `.wd-test` files take no
`compatibilityDate` (wd_test injects `--compat-date`).

## Coverage

`cancelReadableSettlesSocket` pins one implementation divergence: canceling
a pending read rejects with a re-created `Error` carrying the cancel reason
under C++, while TypeScript resolves the read done.

| Test | Shape |
| --- | --- |
| `echoRoundTrip` | write ×2, half-close via writer.close(), drain echo to EOF |
| `degenerateViewsWithHighWaterMark` | byte-counting writable (`highWaterMark`): detached/out-of-bounds typed-array and DataView views count and send nothing; the stream keeps writing |
| `stringSizesWithHighWaterMark` | byte-counting writable: a string counts its UTF-8 bytes (multi-byte sequences; 3 per lone surrogate, sent as U+FFFD) |
| `greetReadsToEof` | server-initiated EOF: greeting then done, tail read `{done: true, value: undefined}` |
| `echoByobReads` | BYOB reader with recycled views over the socket readable, byte-exact |
| `echoReadAtLeast` | readAtLeast accumulates across TCP fragmentation |
| `pipeSocketReadableToJsSink` | socket → JS WritableStream via pipeTo |
| `pipeJsSourceToSocketWritable` | JS ReadableStream → socket writable, echo drained concurrently |
| `pipeSocketThroughJsTransform` | socket → JS TransformStream → JS sink |
| `pipeSocketToSocket` | greet socket's readable piped into the echo socket's writable |
| `pipeSocketToSocketClosesSource` | the same native-to-native pipe leaves the source locked and (TS, via the interop closed-promise) closed at the start |
| `pipeBehindUnawaitedWrite` | header write not awaited, writer released, Response body piped in: echo is header then body; both endpoints unlocked after the pipe |
| `pipeBehindWriteBeforeStart` | the same within connect()'s turn, with the header still queued before the writable starts |
| `cancelReadableSettlesSocket` | reader.cancel settles a pending peer read (C++ rejects; TS resolves done), then socket.close()/closed settle |
| `detachRejectsPendingWrites` | writes pending at startTls()'s detach (one in flight to the stall server, two queued) all reject with the same `Network connection lost.` error; the writable stays locked and (TS, via the interop closed-promise) is closed |
| `detachClosesIdleWritable` | startTls() with nothing pending: the writable is locked and (TS) closed at the detach |
| `largeEchoVolume` | 256 KiB continuous pattern, concurrent producer/consumer, byte-exact |
