// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Generic "outbound interceptor" socket-transfer test.
//
// This mirrors a proxy/interceptor service that, rather than connecting to a backend itself, obtains
// a socket from a downstream service over JS RPC and returns it to its own caller. The returned
// Socket is therefore transferred across TWO RPC hops (backend connect() handler -> interceptor ->
// client), exercising Socket::serialize()/deserialize() at each hop.
import { WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';

// Downstream backend, reached via `env.BACKEND.connect()`. Echoes everything written back to the
// reader so the client can prove both directions of the transferred socket survive.
export class Backend extends WorkerEntrypoint {
  async connect(socket) {
    await socket.readable.pipeTo(socket.writable);
  }
}

// The interceptor. Mirrors the outbound-interceptor structure (a `host` check that routes "magic"
// addresses to a specific backend), but returns the backend's socket over RPC rather than piping it
// -- which is the pattern whose lifetime we want to validate.
export class OutboundInterceptor extends WorkerEntrypoint {
  async interceptConnect(host) {
    // `this.env.BACKEND.connect(host)` does NOT call Backend's `connect(socket)` handler as an
    // RPC method. `connect` is a reserved name on a service binding (Fetcher): this invokes the
    // built-in client-side socket API `Fetcher::connect` (C++: `jsg::Ref<Socket> connect(...)` in
    // api/http.{h,c++} -> connectImpl in api/sockets.c++), which RETURNS the *client* end of a new
    // connection to the BACKEND service. workerd then dispatches the *server* end of that same
    // connection to Backend's exported `connect(socket)` ingress handler (the echo above). So there
    // are two ends of one pipe: what we return here is the client end.
    return this.env.BACKEND.connect(host);
  }
}

async function echoRoundTrip(socket, payload) {
  await socket.opened;
  const enc = new TextEncoder();
  const dec = new TextDecoder();
  const writer = socket.writable.getWriter();
  await writer.write(enc.encode(payload));
  await writer.close();
  let result = '';
  for await (const chunk of socket.readable) {
    result += dec.decode(chunk, { stream: true });
  }
  result += dec.decode();
  return result;
}

export let interceptorForwardsSocket = {
  async test(ctrl, env) {
    // interceptConnect() returns a socket obtained from the backend over RPC (backend -> interceptor);
    // awaiting it here transfers it a second hop (interceptor -> client). If the producer context is
    // not kept alive past interceptConnect()'s return, the echo round-trip below would hang.
    const socket = await env.INTERCEPTOR.interceptConnect('backend:5432');
    const echoed = await echoRoundTrip(socket, 'ping');
    assert.strictEqual(echoed, 'ping');
  },
};
