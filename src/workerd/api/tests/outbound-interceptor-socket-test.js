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

// Downstream backend that writes a marker and then half-closes: it closes only its write side (so
// the client sees read EOF) while keeping its read side open by draining it. This lets the client
// observe whether the transferred socket honors `allowHalfOpen` (i.e. whether the client auto-closes
// its own write side once its read side hits EOF).
export class HalfCloseBackend extends WorkerEntrypoint {
  async connect(socket) {
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('bye'));
    await writer.close();
    // Keep the read side open, draining anything the client sends until it closes its write side.
    for await (const _chunk of socket.readable) {
      // discard
    }
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
    const socket = this.env.BACKEND.connect(host);
    // A Socket can only be serialized for RPC once its connection has been established, so we must
    // await `opened` before returning it. Socket::serialize() is synchronous and cannot await this
    // itself; transferring a still-connecting socket throws a DataCloneError.
    await socket.opened;
    return socket;
  }

  // Returns a socket WITHOUT awaiting `opened` first. Serializing it for the RPC response must throw
  // a DataCloneError, since the connection state (and SocketInfo) isn't yet settled.
  interceptConnectWithoutAwait(host) {
    return this.env.BACKEND.connect(host);
  }

  // Connects to the half-close backend with the given `allowHalfOpen` option and transfers the
  // socket to the caller. The option must survive serialization so the transferred socket behaves
  // the same as a locally-created one.
  async interceptConnectHalfOpen(host, allowHalfOpen) {
    const socket = this.env.HALFCLOSE.connect(host, { allowHalfOpen });
    await socket.opened;
    return socket;
  }
}

async function readToEnd(socket) {
  const dec = new TextDecoder();
  let result = '';
  for await (const chunk of socket.readable) {
    result += dec.decode(chunk, { stream: true });
  }
  result += dec.decode();
  return result;
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

export let transferringUnopenedSocketThrows = {
  async test(ctrl, env) {
    // A socket that hasn't finished connecting cannot be serialized for RPC. The interceptor returns
    // one without awaiting `opened`, so serializing the RPC response must fail with a DataCloneError.
    await assert.rejects(
      env.INTERCEPTOR.interceptConnectWithoutAwait('backend:5432'),
      {
        name: 'DataCloneError',
      }
    );
  },
};

export let transferredSocketHonorsAllowHalfOpenFalse = {
  async test(ctrl, env) {
    // With allowHalfOpen=false (the default), a transferred socket must auto-close its write side
    // once the read side reaches EOF. The backend half-closes after sending "bye", so reading to EOF
    // triggers the auto-close, which resolves `closed`.
    const socket = await env.INTERCEPTOR.interceptConnectHalfOpen(
      'halfclose:1',
      false
    );
    assert.strictEqual(await readToEnd(socket), 'bye');
    // If allowHalfOpen were not carried across transfer, the write side would stay open and `closed`
    // would hang here.
    await socket.closed;
  },
};

export let transferredSocketHonorsAllowHalfOpenTrue = {
  async test(ctrl, env) {
    // With allowHalfOpen=true, the write side must stay open after the read side reaches EOF, so we
    // can still write to the (still-reading) backend afterwards.
    const socket = await env.INTERCEPTOR.interceptConnectHalfOpen(
      'halfclose:1',
      true
    );
    assert.strictEqual(await readToEnd(socket), 'bye');
    // The write side must not have been auto-closed; writing after read EOF must succeed. If the
    // transferred socket wrongly defaulted to allowHalfOpen=false, this write would throw.
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('still-open'));
    await writer.close();
  },
};

export let transferredSocketClosedResolvesOnExplicitClose = {
  async test(ctrl, env) {
    // A transferred socket must still support explicit close(): calling it resolves `closed`.
    // allowHalfOpen=true so nothing auto-closes the socket for us -- only the explicit close() does.
    const socket = await env.INTERCEPTOR.interceptConnectHalfOpen(
      'halfclose:1',
      true
    );
    await socket.close();
    await socket.closed;
  },
};
