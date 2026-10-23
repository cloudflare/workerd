// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A throwing client WebSocket 'open' listener closes the incoming side while the read loop
// may still be running. Releasing the native connection must stop that loop safely, and
// cleanup must not depend on the peer disconnecting TCP.
//
// The abort test holds a Durable Object's input gate while the peer replies and then resets
// TCP during an outgoing write. The read loop waits in IoContext::run() while the native
// connection is released; when the gate opens, it must discard the reply without touching
// the released socket.
//
// The Close and silent-peer tests let outgoing writes finish and leave TCP open at the peer.
// They check that workerd releases the connection without waiting for a peer disconnect or
// context teardown. With a silent peer, the read loop's receive() is still pending when the
// connection is released, so the release must cancel it.

import assert from 'node:assert';

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export class OpenErrorClient {
  constructor(state, env) {
    this.state = state;
    this.env = env;
  }

  async fetch() {
    const ws = new WebSocket(
      `ws://${this.env.SIDECAR_HOSTNAME}:${this.env.OPEN_ERROR_SERVER_PORT}/`
    );
    const events = [];

    const closed = new Promise((resolve) => {
      ws.addEventListener('open', () => {
        events.push('open');
        // Start the outgoing pump so the error path doesn't release the native socket
        // immediately. The server stops reading after 'ping', so the large message stays in
        // the pump until the server resets the connection.
        ws.send('ping');
        ws.send(new Uint8Array(32 << 20));
        // Hold the input gate, so that the read loop has to wait to deliver the server's reply
        // while the connection is aborted.
        this.state.blockConcurrencyWhile(() => sleep(1000));
        throw new Error('expected error thrown from open listener');
      });
      ws.addEventListener('message', (event) => {
        events.push(`message:${event.data}`);
      });
      ws.addEventListener('error', () => {
        events.push('error');
      });
      ws.addEventListener('close', (event) => {
        events.push(`close:${event.code}`);
        resolve();
      });
    });

    await closed;
    // Wait until blockConcurrencyWhile() has finished and the read loop has run again.
    await sleep(1500);

    return Response.json({ events, readyState: ws.readyState });
  }
}

async function expectReleaseWhilePeerKeepsTcpOpen(env, path, closeSent) {
  const origin = `${env.SIDECAR_HOSTNAME}:${env.OPEN_ERROR_SERVER_PORT}`;
  const id = crypto.randomUUID();
  const ws = new WebSocket(`ws://${origin}${path}?id=${id}`);
  const events = [];
  const closed = new Promise((resolve) => {
    ws.addEventListener('open', () => {
      events.push('open');
      // The pump is still active when the listener throws, so the error path cannot release
      // the native socket until the pump finishes.
      ws.send('ping');
      throw new Error('expected error thrown from open listener');
    });
    ws.addEventListener('message', () => {
      events.push('message');
    });
    ws.addEventListener('error', () => {
      events.push('error');
    });
    ws.addEventListener('close', (event) => {
      events.push(`close:${event.code}`);
      resolve();
    });
  });

  await closed;
  // The JS close event alone does not prove release. Keep this context alive until the
  // sidecar observes workerd initiating TCP shutdown.
  const response = await fetch(`http://${origin}/closed?id=${id}`);
  const result = await response.json();
  assert.strictEqual(response.status, 200, JSON.stringify(result));
  assert.deepStrictEqual(result, { closed: true, closeSent });
  assert.strictEqual(ws.readyState, WebSocket.CLOSED);
  assert.deepStrictEqual(events, ['open', 'error', 'close:1006']);
}

export const openListenerThrowsThenPeerCloses = {
  async test(controller, env) {
    await expectReleaseWhilePeerKeepsTcpOpen(env, '/peer-close', true);
  },
};

export const openListenerThrowsThenPeerIsSilent = {
  async test(controller, env) {
    await expectReleaseWhilePeerKeepsTcpOpen(env, '/peer-silent', false);
  },
};

export const openListenerThrowsThenPeerAborts = {
  async test(controller, env) {
    const stub = env.ns.get(env.ns.idFromName('open-error'));
    const res = await stub.fetch('http://do/');
    const { events, readyState } = await res.json();

    assert.strictEqual(readyState, WebSocket.CLOSED);
    assert.deepStrictEqual(events, ['open', 'error', 'close:1006']);
  },
};
