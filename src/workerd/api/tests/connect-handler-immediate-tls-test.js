// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Coverage for a connect() handler serving a peer that asks for secureTransport 'on' over a
// service binding. The handlers are in connect-handler-test-proxy.js.
import { ok, rejects, strictEqual } from 'assert';

async function readAll(socket) {
  const dec = new TextDecoder();
  let result = '';
  for await (const chunk of socket.readable) {
    result += dec.decode(chunk, { stream: true });
  }
  return result + dec.decode();
}

// Writes `text` without waiting for the socket to open. The write is held until the handler
// accepts the connection.
function writeEarly(socket, text) {
  const writer = socket.writable.getWriter();
  const written = writer.write(new TextEncoder().encode(text));
  writer.releaseLock();
  return written;
}

// The handler opens a TLS connection to a TCP listener that terminates TLS with a certificate
// the handler's network trusts, so the handshake is real, and then relays. The listener's reply
// proves that the bytes written before the socket opened went over that connection.
export const immediateTlsThroughRelay = {
  async test(ctrl, env) {
    const socket = env.RELAY.connect('db.example:5432', {
      secureTransport: 'on',
    });
    strictEqual(socket.secureTransport, 'on');
    const written = writeEarly(socket, 'ping\r\n');
    await socket.opened;
    await written;
    strictEqual(await readAll(socket), 'secure:ping\r\n');
  },
};

export const immediateTlsHoldsWritesUntilAccepted = {
  async test(ctrl, env) {
    const socket = env.HOLDS_WRITES.connect('db.example:5432', {
      secureTransport: 'on',
    });
    const written = writeEarly(socket, 'early');
    await socket.opened;
    await written;
    strictEqual(await readAll(socket), 'got:early');
  },
};

// Each of these handlers fails to accept the connection. The peer's socket never opens, and the
// write it made in the meantime is never read. A local service binding's pipe holds no bytes of
// its own, so the write fails too; a transport that buffers, such as capnp between processes, may
// accept the write before the connection fails.
async function expectRefused(binding) {
  const socket = binding.connect('db.example:5432', { secureTransport: 'on' });
  const written = writeEarly(socket, 'must-not-be-sent');
  await rejects(socket.opened, {
    message: 'proxy request failed, cannot connect to the specified address',
  });
  await rejects(written);
}

export const immediateTlsRefusedWhenHandshakeFails = {
  async test(ctrl, env) {
    await expectRefused(env.FAILED_HANDSHAKE);
  },
};

export const immediateTlsRefusedWhenNeverAccepted = {
  async test(ctrl, env) {
    await expectRefused(env.NEVER_ACCEPTS);
  },
};

export const immediateTlsRefusedWhenHandlerClosesFirst = {
  async test(ctrl, env) {
    await expectRefused(env.CLOSES_FIRST);
  },
};

// A handler without connect_handler_immediate_tls can't make the connection secure, so the
// connection is refused before it runs.
export const immediateTlsRefusedWithoutFlag = {
  async test(ctrl, env) {
    // Whether the refusal surfaces from connect() itself or from `opened` depends on whether the
    // binding delivers the CONNECT synchronously. A local service binding does.
    let socket;
    try {
      socket = env.WITHOUT_FLAG.connect('db.example:5432', {
        secureTransport: 'on',
      });
    } catch (e) {
      strictEqual(e.message, 'Incoming CONNECT with TLS not supported');
      return;
    }
    await rejects(socket.opened);
  },
};

export const plaintextPeerAcceptedOnDelivery = {
  async test(ctrl, env) {
    const socket = env.PLAINTEXT_PEER.connect('db.example:5432');
    const info = await socket.opened;
    ok(info);
    strictEqual(await readAll(socket), 'plaintext-ok');
  },
};
