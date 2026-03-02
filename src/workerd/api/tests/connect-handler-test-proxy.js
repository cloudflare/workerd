// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { connect } from 'cloudflare:sockets';
import { WorkerEntrypoint } from 'cloudflare:workers';
import { ok, strictEqual } from 'assert';

export class ConnectProxy extends WorkerEntrypoint {
  async connect(socket) {
    // proxy for ConnectEndpoint instance on port 8083.
    let upstream = connect('localhost:8083');
    socket.proxyTo(upstream);
    // proxyTo() can't be awaited – wait briefly so that we can be sure the data has been sent by
    // the time we return so that the calling worker can read it right away.
    await scheduler.wait(10);
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
// connect-handler-test.js.
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
