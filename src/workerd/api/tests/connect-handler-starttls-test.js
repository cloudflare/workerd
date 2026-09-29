// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// startTls() coverage for the connect() handler, on both transports that can deliver a CONNECT to
// a handler: a TCP listener configured with a keypair, and a service binding.
import { connect } from 'cloudflare:sockets';
import { WorkerEntrypoint } from 'cloudflare:workers';
import { ok, strictEqual } from 'assert';

// Reads from `reader` until a full CRLF-terminated line is available and returns it, line ending
// included. The miniature STARTTLS protocol below is line-based and strictly alternating, so a
// single read almost always yields exactly one line, but nothing about the stream guarantees that.
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

// Exercises startTls() on the TCP listener path. The listener's socket carries a keypair, so both
// ends run a real handshake: startTls() on the the handler side serves it and this side's
// startTls() verifies the certificate.
export const startTlsViaTcpListener = {
  async test() {
    const socket = connect('localhost:8084', { secureTransport: 'starttls' });
    await socket.opened;

    const tlsSock = socket.startTls();
    const dec = new TextDecoder();
    let result = '';
    for await (const chunk of tlsSock.readable) {
      result += dec.decode(chunk, { stream: true });
    }
    result += dec.decode();
    strictEqual(result, 'hello-over-tls');

    await tlsSock.closed;
  },
};

// Speaks the client side of the miniature STARTTLS protocol that StartTlsEndpoint (in
// connect-handler-test-proxy.js) and StartTlsServer below serve.
async function speakStartTlsClient(socket) {
  const enc = new TextEncoder();
  await socket.opened;

  let reader = socket.readable.getReader();
  let writer = socket.writable.getWriter();
  strictEqual(await readLine(reader), '220 ready\r\n');
  await writer.write(enc.encode('STARTTLS\r\n'));
  strictEqual(await readLine(reader), '220 go ahead\r\n');

  // startTls() requires the streams to be unlocked: it detaches them from this socket and hands
  // the connection to the socket it returns.
  reader.releaseLock();
  writer.releaseLock();

  const secure = socket.startTls();
  await secure.opened;

  reader = secure.readable.getReader();
  writer = secure.writable.getWriter();
  await writer.write(enc.encode('EHLO client\r\n'));
  strictEqual(await readLine(reader), '250 secure\r\n');
  await writer.close();

  // The service binding's pipe outlives the handler that served it, so nothing reports a
  // write-disconnect here. What ends the upgraded socket is the EOF the server sent by closing
  // its own writer, and reading it is what settles `closed`.
  strictEqual((await reader.read()).done, true);
  reader.releaseLock();
  await secure.closed;
}

// Exercises startTls() on the service-binding path, where neither end of the tunnel can perform a
// TLS handshake: the bytes never leave the runtime. Both ends still have to call startTls(),
// because the protocol they speak announces the upgrade in-band and each end swaps its stream over
// to a fresh Socket at that point in the stream.
export const startTlsViaServiceBinding = {
  async test(ctrl, env) {
    await speakStartTlsClient(
      env.STARTTLS_TARGET.connect('smtp.example.com:25', {
        secureTransport: 'starttls',
      })
    );
  },
};

// Exercises startTls() through a connect() handler that does nothing but proxyTo() an upstream
// socket. The handler never calls startTls(): the client's call is forwarded to the upstream
// socket, which runs a real handshake with StartTlsServer, verified against the certificate the
// `internet` service trusts. The client is the same one startTlsViaServiceBinding uses, so it
// cannot tell the two apart.
export const startTlsForwardedByProxyTo = {
  async test(ctrl, env) {
    await speakStartTlsClient(
      env.STARTTLS_PROXY.connect('smtp.example.com:25', {
        secureTransport: 'starttls',
      })
    );
  },
};

// Serves the TCP listener on port 8085 for startTlsForwardedByProxyTo: the protocol
// StartTlsEndpoint serves, with a real handshake at the upgrade.
export class StartTlsServer extends WorkerEntrypoint {
  async connect(socket) {
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
    reader = secure.readable.getReader();
    writer = secure.writable.getWriter();
    strictEqual(await readLine(reader), 'EHLO client\r\n');
    await writer.write(enc.encode('250 secure\r\n'));

    reader.releaseLock();
    await writer.close();
  }
}

// Serves the TCP listener on port 8084 for startTlsViaTcpListener.
export default {
  async connect(socket) {
    // The listener offers the upgrade because its socket is configured with a keypair.
    strictEqual(socket.secureTransport, 'starttls');

    const tlsSock = socket.startTls();
    const writer = tlsSock.writable.getWriter();
    await writer.write(new TextEncoder().encode('hello-over-tls'));
    await writer.close();
  },
};
