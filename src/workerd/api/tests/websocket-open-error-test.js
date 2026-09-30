// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test: once a client WebSocket's 'open' listener throws, the native kj::WebSocket
// must not be released while the read loop can still use it.
//
// internalAccept() starts the read loop and then dispatches 'open'. If an 'open' listener
// throws, the error path sets `closedIncoming = true` even though the read loop is still
// running. The whenAborted() task treats `closedIncoming && !isPumping` as proof that the read
// loop is done with the socket and destroys it. The read loop holds a raw reference to that
// socket.
//
// The test builds the following sequence inside a Durable Object:
//
// 1. The 'open' listener queues a small and a large message, which sets `isPumping`, so the
//    error path's reportError() does not release the socket right away. It then closes the
//    input gate with blockConcurrencyWhile() and throws.
// 2. The server reads the small message, stops reading, and replies. The read loop's
//    receive() completes, and the read loop waits in IoContext::run() for the input gate.
// 3. The server resets the connection while the large message is still being written. The
//    pump fails (`isPumping` becomes false), the network side drops its end of the
//    connection, and whenAborted() fires. The abort task destroys the native socket.
// 4. The input gate opens. The read loop delivers the message and calls receive() on the
//    destroyed socket. Without a fix, ASAN reports heap-use-after-free in
//    LegacyWebSocketAdapter::readLoop().

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

export const openListenerThrowsThenPeerAborts = {
  async test(controller, env) {
    const stub = env.ns.get(env.ns.idFromName('open-error'));
    const res = await stub.fetch('http://do/');
    const { events, readyState } = await res.json();

    assert.strictEqual(readyState, WebSocket.CLOSED);
    assert.deepStrictEqual(events, ['open', 'error', 'close:1006']);
  },
};
