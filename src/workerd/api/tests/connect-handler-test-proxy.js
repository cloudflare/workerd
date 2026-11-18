// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { connect } from 'cloudflare:sockets';
import { WorkerEntrypoint } from 'cloudflare:workers';
import { ok, rejects, strictEqual, throws } from 'assert';

export class ConnectProxy extends WorkerEntrypoint {
  async connect(socket) {
    // proxy for the ConnectEndpoint instance on the connect-endpoint socket.
    const upstream = connect(
      `${this.env.SIDECAR_HOSTNAME}:${this.env.CONNECT_ENDPOINT_PORT}`
    );
    // The handler's socket is closed when the handler returns, so wait for the relay to finish.
    await socket.proxyTo(upstream);
  }
}

export class ConnectEndpoint extends WorkerEntrypoint {
  async connect(socket) {
    const enc = new TextEncoder();
    let writer = socket.writable.getWriter();
    await writer.write(enc.encode('hello-from-endpoint'));
    await writer.close();
  }
}

// Reads a full CRLF-terminated line, line ending included. See readLine() in
// connect-handler-starttls-test.js.
async function readLine(reader) {
  const dec = new TextDecoder();
  let line = '';
  while (!line.endsWith('\r\n')) {
    const { value, done } = await reader.read();
    ok(!done, `peer closed mid-line: ${line}`);
    line += dec.decode(value, { stream: true });
  }
  return line;
}

// Reached via a service binding from connect-handler-test.js. Serves a miniature line-based
// protocol that negotiates TLS in-band, the way SMTP and Postgres do: it greets the client,
// acknowledges the client's upgrade request, and then upgrades its own end of the socket. Neither
// end of a service-binding tunnel can run a handshake, so startTls() here resolves once the client
// has called startTls() too.
export class StartTlsEndpoint extends WorkerEntrypoint {
  async connect(socket) {
    // The tunnel came from another Socket, so it can carry an upgrade.
    strictEqual(socket.secureTransport, 'starttls');

    const enc = new TextEncoder();
    let reader = socket.readable.getReader();
    let writer = socket.writable.getWriter();
    await writer.write(enc.encode('220 ready\r\n'));
    strictEqual(await readLine(reader), 'STARTTLS\r\n');
    await writer.write(enc.encode('220 go ahead\r\n'));

    reader.releaseLock();
    writer.releaseLock();

    const secure = socket.startTls();
    await secure.opened;

    reader = secure.readable.getReader();
    writer = secure.writable.getWriter();
    strictEqual(await readLine(reader), 'EHLO client\r\n');
    await writer.write(enc.encode('250 secure\r\n'));

    reader.releaseLock();
    await writer.close();
  }
}

// Reached via a service binding from connect-handler-test.js. Awaits socket.opened and echoes back
// the observed addresses. On the service-binding path localAddress is the verbatim authority
// string the caller passed to fetcher.connect(...), and no client IP is supplied.
export class LocalAddressEndpoint extends WorkerEntrypoint {
  async connect(socket) {
    const { localAddress, remoteAddress } = await socket.opened;
    const enc = new TextEncoder();
    const writer = socket.writable.getWriter();
    await writer.write(
      enc.encode(
        JSON.stringify({
          localAddress: localAddress ?? null,
          remoteAddress: remoteAddress ?? null,
        })
      )
    );
    await writer.close();
  }
}

// Serves the connect-tls TCP listener, which terminates TLS before the connection reaches this
// handler. Answers one line with that line prefixed by `secure:`, so that its peer can tell the
// reply came over the TLS connection.
export class TlsEchoEndpoint extends WorkerEntrypoint {
  async connect(socket) {
    const reader = socket.readable.getReader();
    const line = await readLine(reader);
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode(`secure:${line}`));
    await writer.close();
  }
}

// The handlers below are reached through service bindings from
// connect-handler-immediate-tls-test.js, whose sockets ask for secureTransport 'on'. Neither end of
// a service binding can run a handshake, so each handler makes the connection secure further
// along, or fails to, before it accepts the connection.

// The shape a binding Worker in front of a private network has: open the upstream with TLS, wait
// for the handshake, accept, and relay.
export class ImmediateTlsRelay extends WorkerEntrypoint {
  async connect(socket) {
    // The peer asked for TLS from the start, so the handler has to accept the connection once it
    // is secure. The handler's own socket carries plaintext, which is what the peer writes and
    // reads.
    strictEqual(socket.secureTransport, 'on');

    const upstream = connect(
      `${this.env.SIDECAR_HOSTNAME}:${this.env.TLS_PORT}`,
      { secureTransport: 'on' }
    );
    await upstream.opened;
    // Only the socket passed to a connect() handler has a connection to accept.
    throws(() => upstream.accept(), {
      name: 'TypeError',
      message:
        'accept() can only be called on the socket passed to a connect() handler whose ' +
        "peer requested secureTransport 'on'.",
    });
    socket.accept();
    // A second accept() changes nothing.
    socket.accept();
    await socket.proxyTo(upstream);
  }
}

// Checks that nothing the peer writes reaches the handler before the handler accepts the
// connection, and that it all arrives afterwards.
export class ImmediateTlsHoldsWrites extends WorkerEntrypoint {
  async connect(socket) {
    const reader = socket.readable.getReader();
    const firstRead = reader.read();
    // The peer wrote before its socket opened. If those bytes were let through, they would arrive
    // well within this wait; that they don't is what accept() guarantees, so a slow run can't make
    // this fail spuriously.
    const early = await Promise.race([
      firstRead.then(() => 'data'),
      scheduler.wait(50).then(() => 'nothing'),
    ]);
    strictEqual(early, 'nothing');

    socket.accept();
    const { value, done } = await firstRead;
    ok(!done);
    const writer = socket.writable.getWriter();
    await writer.write(
      new TextEncoder().encode(`got:${new TextDecoder().decode(value)}`)
    );
    await writer.close();
  }
}

// Secures the connection through a protocol of the application's own on an upstream that the
// runtime sees as plaintext, the way a tunnel that runs TLS inside its own framing does. The
// runtime can't tell that the upstream is secure, so it leaves that to the handler: accept() and
// proxyTo() don't look at the upstream's secureTransport.
export class ImmediateTlsOverCustomProtocol extends WorkerEntrypoint {
  async connect(socket) {
    strictEqual(socket.secureTransport, 'on');
    const upstream =
      this.env.CUSTOM_PROTOCOL_UPSTREAM.connect('db.example:5432');
    strictEqual(upstream.secureTransport, 'off');

    const reader = upstream.readable.getReader();
    const writer = upstream.writable.getWriter();
    await writer.write(new TextEncoder().encode('SECURE\r\n'));
    strictEqual(await readLine(reader), 'SECURED\r\n');
    reader.releaseLock();
    writer.releaseLock();

    socket.accept();
    await socket.proxyTo(upstream);
  }
}

// The upstream ImmediateTlsOverCustomProtocol secures its connection through. It stands in for
// the far end of a protocol that runs TLS inside its own framing: it answers the handler's
// request to secure the connection, then answers one line with that line prefixed by `tunneled:`.
export class CustomProtocolEndpoint extends WorkerEntrypoint {
  async connect(socket) {
    strictEqual(socket.secureTransport, 'off');
    const reader = socket.readable.getReader();
    const writer = socket.writable.getWriter();
    const enc = new TextEncoder();
    strictEqual(await readLine(reader), 'SECURE\r\n');
    await writer.write(enc.encode('SECURED\r\n'));
    const line = await readLine(reader);
    await writer.write(enc.encode(`tunneled:${line}`));
    await writer.close();
  }
}

// The handler asks the runtime for TLS to an upstream that isn't a TLS server, so the handshake
// fails, upstream.opened rejects, and the handler throws before accepting.
export class ImmediateTlsFailedHandshake extends WorkerEntrypoint {
  async connect(socket) {
    const upstream = connect(
      `${this.env.SIDECAR_HOSTNAME}:${this.env.CONNECT_ENDPOINT_PORT}`,
      { secureTransport: 'on' }
    );
    await upstream.opened;
    socket.accept();
    await socket.proxyTo(upstream);
  }
}

// Returns without accepting.
export class ImmediateTlsNeverAccepts extends WorkerEntrypoint {
  async connect(socket) {}
}

// Closes its socket before accepting, and finds that the connection can no longer be accepted.
export class ImmediateTlsClosesFirst extends WorkerEntrypoint {
  async connect(socket) {
    await socket.close();
    throws(() => socket.accept(), {
      message:
        "The connect() handler's socket closed before it accepted the connection.",
    });
  }
}

// Throws before returning a promise, after queueing an accept() that runs once connect() has
// unwound. The connection fails with the throw, and the late accept() must not answer the peer:
// by then nothing is waiting for the answer.
export class ImmediateTlsThrowsSynchronously extends WorkerEntrypoint {
  connect(socket) {
    queueMicrotask(() => {
      try {
        socket.accept();
      } catch {
        // Expected: the connection already failed.
      }
    });
    throw new Error('connect() handler failed before accepting');
  }
}

// Tries to pass its socket to another entrypoint before accepting the connection, which fails:
// only this handler can answer the peer. Then accepts and reports the failure to the peer.
export class ImmediateTlsTransfersBeforeAccepting extends WorkerEntrypoint {
  async connect(socket) {
    // The handler's socket opens without waiting for accept(). Transferring it any earlier fails
    // for that reason instead.
    await socket.opened;
    await rejects(this.env.SOCKET_SINK.take(socket), {
      name: 'DataCloneError',
      message:
        'A Socket can only be serialized after its connection has been accepted. Call ' +
        '`socket.accept()` before transferring the socket over RPC.',
    });
    socket.accept();
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('transfer-refused'));
    await writer.close();
  }
}

// The RPC target ImmediateTlsTransfersBeforeAccepting tries to pass its socket to.
export class SocketSink extends WorkerEntrypoint {
  take(socket) {}
}

// A peer that didn't ask for TLS gets its connection accepted on delivery, leaving nothing for
// accept() to do.
export class ImmediateTlsPlaintextPeer extends WorkerEntrypoint {
  async connect(socket) {
    strictEqual(socket.secureTransport, 'off');
    throws(() => socket.accept(), {
      name: 'TypeError',
      message:
        'accept() can only be called on the socket passed to a connect() handler whose ' +
        "peer requested secureTransport 'on'.",
    });
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('plaintext-ok'));
    await writer.close();
  }
}
