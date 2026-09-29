// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { connect } from 'cloudflare:sockets';
import { WorkerEntrypoint } from 'cloudflare:workers';
import { ok, strictEqual } from 'assert';

export class ConnectProxy extends WorkerEntrypoint {
  async connect(socket) {
    // proxy for ConnectEndpoint instance on port 8083.
    const upstream = connect('localhost:8083');
    // The handler's socket is closed when the handler returns, so wait for the relay to finish.
    await socket.proxyTo(upstream);
  }
}

// Reached via a service binding from connect-handler-starttls-test.js. Relays to a server that
// negotiates TLS in-band and then performs a real handshake, without knowing the protocol or where
// in it the upgrade falls. The client's startTls() is forwarded to the upstream socket, which is
// why that socket has to be opened with secureTransport: 'starttls'.
export class StartTlsProxy extends WorkerEntrypoint {
  async connect(socket) {
    const upstream = connect('localhost:8085', { secureTransport: 'starttls' });
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

// Serves the plaintext half of a miniature line-based protocol that negotiates TLS in-band, the
// way SMTP and Postgres do: greets the client and acknowledges its upgrade request. After this,
// the client upgrades its end, and the socket's streams are left unlocked for the handler to do
// the same.
async function serveUpToUpgrade(socket) {
  // The tunnel came from another Socket, so it can carry an upgrade.
  strictEqual(socket.secureTransport, 'starttls');

  const enc = new TextEncoder();
  const reader = socket.readable.getReader();
  const writer = socket.writable.getWriter();
  await writer.write(enc.encode('220 ready\r\n'));
  strictEqual(await readLine(reader), 'STARTTLS\r\n');
  await writer.write(enc.encode('220 go ahead\r\n'));

  reader.releaseLock();
  writer.releaseLock();
}

// Reached via a service binding from connect-handler-starttls-test.js. Serves the protocol
// serveUpToUpgrade() begins, and then upgrades its own end of the socket. Neither end of a
// service-binding tunnel can run a handshake, so startTls() here resolves once the client has
// called startTls() too.
export class StartTlsEndpoint extends WorkerEntrypoint {
  async connect(socket) {
    await serveUpToUpgrade(socket);

    const enc = new TextEncoder();
    const secure = socket.startTls();
    await secure.opened;

    const reader = secure.readable.getReader();
    const writer = secure.writable.getWriter();
    strictEqual(await readLine(reader), 'EHLO client\r\n');
    await writer.write(enc.encode('250 secure\r\n'));

    reader.releaseLock();
    await writer.close();
  }
}

// Reached via a service binding from connect-handler-starttls-test.js. Acknowledges the client's
// upgrade request, and then returns without upgrading its own end.
export class FinishesWithoutStartTls extends WorkerEntrypoint {
  async connect(socket) {
    await serveUpToUpgrade(socket);
  }
}

// Reached via a service binding from connect-handler-starttls-test.js. Acknowledges the client's
// upgrade request, and then closes its socket instead of upgrading it.
export class ClosesWithoutStartTls extends WorkerEntrypoint {
  async connect(socket) {
    await serveUpToUpgrade(socket);
    await socket.close();
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
