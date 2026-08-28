# connect() sockets × streams

The readable/writable stream halves of `connect()` TCP sockets under
both stream implementations. **The tests are the normative artifact.**
The general Socket surface (startTls, secureTransport, DNS overrides,
the connect-handler protocol, HTTP-over-socket) is owned by
`src/workerd/api/tests/` (http-socket-test, connect-handler-test,
starttls-*); this suite owns the STREAMS interaction only.

## Infrastructure

A node sidecar (echo-server.js) runs two TCP servers, their ports
delivered through `fromEnvironment` bindings (`STREAMS_ECHO_PORT`,
`STREAMS_GREET_PORT`, plus `SIDECAR_HOSTNAME`):

- **echo**: echoes every byte; on client half-close, flushes and ends
  (the client's readable reaches EOF after the full echo).
- **greet**: writes one fixed message and ends immediately.

Both cells need `experimental` (Socket) and an `internet` network
service allowing `private`. The suite's `.wd-test` files take no
`compatibilityDate` (wd_test injects `--compat-date`).

## Coverage (parity — no divergences observed)

| Test | Shape |
| --- | --- |
| `echoRoundTrip` | write ×2, half-close via writer.close(), drain echo to EOF |
| `greetReadsToEof` | server-initiated EOF: greeting then done, tail read `{done: true, value: undefined}` |
| `echoByobReads` | BYOB reader with recycled views over the socket readable, byte-exact |
| `echoReadAtLeast` | readAtLeast accumulates across TCP fragmentation |
| `pipeSocketReadableToJsSink` | socket → JS WritableStream via pipeTo |
| `pipeJsSourceToSocketWritable` | JS ReadableStream → socket writable, echo drained concurrently |
| `pipeSocketThroughJsTransform` | socket → JS TransformStream → JS sink |
| `pipeSocketToSocket` | greet socket's readable piped into the echo socket's writable |
| `cancelReadableSettlesSocket` | reader.cancel mid-stream, then socket.close() settles |
| `largeEchoVolume` | 256 KiB continuous pattern, concurrent producer/consumer, byte-exact |
