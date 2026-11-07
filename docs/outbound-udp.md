# Experimental outbound UDP sockets

Implementation draft: enable `outbound_udp_sockets` and run workerd with `--experimental`.
This adds outbound UDP to `cloudflare:sockets` using the same Socket object as ingress.
UDP streams carry Uint8Array values; no Datagram wrapper is required.

```js
import { connect } from 'cloudflare:sockets';

const socket = connect(
  { hostname: 'metrics.example.com', port: 8125 },
  { protocol: 'udp' },
);
const writer = socket.writable.getWriter();
try {
  await writer.write(new TextEncoder().encode('requests:1|c'));
} finally {
  writer.releaseLock();
  await socket.close().catch(() => {});
}
```

Each write sends one packet, including a zero-length Uint8Array. Sliced arrays send only
their visible bytes. Bytes are copied before leaving the isolate lock. Writes may be
issued before `opened` resolves; transport use waits for setup and the DO output gate.
The draft rejects payloads larger than 65,507 bytes before copying them. The path and
destination can impose smaller limits or drop fragmented packets.

## Payload limits and write errors

The 65,507-byte limit applies to standalone workerd's native backend. An embedding
runtime may enforce a smaller payload limit. Check the limit for the deployment where
the code runs; a successful standalone test does not establish the embedding's limit.
Limits count `Uint8Array.byteLength`, including only the visible bytes of a slice.
Each datagram is checked independently. For text, measure the encoded bytes, not the
JavaScript string's length.

Await `writer.write(packet)` to observe a size or transport error. A rejected write
errors the writable stream: `writer.closed` and subsequent writes reject with that
error. Close the socket and create a new one before sending more packets. Validate
packet sizes before writing if the application needs to keep using the same stream.
Cleanup should not replace the original write error, hence the caught close rejection
in the example. Packets are never automatically split into smaller datagrams or
silently truncated to the payload limit.

## Socket behavior

`opened` means the runtime's routed UDP flow is ready. A write completing means local
acceptance by the egress path. Neither acknowledges delivery to the destination. The
send-only example does not read, wait for a reply, or assume one will arrive.

`readable` remains available for applications whose destination independently sends
packets back. It is a value stream of Uint8Array packets; it does not support BYOB readers.
A blocked receive does not prevent writes. UDP provides no peer-close notification;
`closed` represents local flow teardown or a transport failure, rather than the peer
closing a connection. Applications should close the socket explicitly when done.

The destination is fixed for the lifetime of the socket. There is no per-packet address,
listener, local bind option, multicast, broadcast option, DTLS, or hibernation support.
`secureTransport`, a true `allowHalfOpen`, and `highWaterMark` are rejected for UDP.
TCP remains the default. Enable the compatibility flag explicitly: without it the new
protocol option is not interpreted and existing TCP behavior applies.

With this flag, ingress UDP sockets also use Uint8Array stream values. The older
Datagram-based ingress experiment remains available when this flag is disabled.

## Routing and lifetime

The socket opens a UdpEgressCustomEvent through its normal subrequest WorkerInterface.
A network backend resolves the destination and checks its configured PeerFilter before
creating a connected native UDP socket. Unsupported services reject the event.

EventDispatcher.openUdp carries the open across runtime RPC. Each direction uses
DatagramStream capabilities, preserving record boundaries and consumption backpressure.
Outbound writes use a non-streaming `sendWithAck` RPC so each write waits for the
backend to accept that packet and receives its transport error. This acknowledgment
is internal to the runtime; the UDP destination sends no acknowledgment.
The returned send capability owns the backend channel and request lifetime. The reverse
capability holds a weak reference to the caller's channel, avoiding a capability cycle.
Dropping the owning channel releases the transport and request accounting state.

The standalone server implements native UDP in its Rust network service. It does not
route UDP through the HTTP/TCP connection pool. Hostname resolution happens once per
flow; the selected address is checked before connect. Reopening re-runs resolution.

## Testing

`src/workerd/server/tests/udp-egress` starts an independent UDP collector and a workerd
subprocess. It checks Worker and Durable Object sends, empty/binary/sliced/larger packets,
an optional echo, value-mode reads and a destination denied by the network policy.
The send-only cases assert receipt at the collector and never read from the socket.

```sh
bazel test //src/workerd/server/tests/udp-egress:udp-egress-test \
  //src/workerd/api:sockets-test@ --runs_per_test=5 --test_output=errors
```

When workerd is a submodule, run tests from the embedding workspace and qualify targets
with `@workerd`. The embedding runtime must provide its own egress transport and tests;
the standalone network backend does not grant embedded Workers native socket access.

Regenerate TypeScript definitions with the repository's `generate-types` recipe and
review the compatibility delta. `protocol` belongs in the experimental SocketOptions;
the stable definition must remain unchanged. Run the embedding runtime's global-scope
snapshot tests as well. Generated files must come from these recipes.
