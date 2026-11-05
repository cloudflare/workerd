// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';
import assert from 'node:assert';

const LIMIT = 32768;
const LIMIT_MESSAGE = `only ${LIMIT} non-hibernatable websockets can be open on a single Durable Object instance`;

export class WebSocketLimitObject extends DurableObject {
  // Every WebSocket the test creates is held here, so that none is garbage-collected. A collected
  // WebSocket, or the collected peer of an accepted one, disconnects and gives its slot back.
  #connections = []; // Opened with `new WebSocket(url)`.
  #pairs = []; // Created with `new WebSocketPair()`, as { client, server }.

  // Opens a WebSocket with `new WebSocket(url)`, through the global outbound below, and keeps it
  // even if it fails, so that only the runtime can close its connection. Resolves to the WebSocket
  // once it opens, or rejects with the error it reports if the connection fails.
  #connect() {
    const ws = new WebSocket('ws://example.com/');
    this.#connections.push(ws);
    return new Promise((resolve, reject) => {
      ws.addEventListener('open', () => resolve(ws));
      ws.addEventListener('error', (event) => reject(event.error));
    });
  }

  // Creates a WebSocketPair, accepting neither end.
  #pair() {
    const [client, server] = Object.values(new WebSocketPair());
    const pair = { client, server };
    this.#pairs.push(pair);
    return pair;
  }

  // Creates a WebSocketPair and accepts both ends, which uses two slots.
  #acceptPair() {
    const pair = this.#pair();
    pair.client.accept();
    pair.server.accept();
    return pair;
  }

  async run() {
    // Fill the limit with two WebSockets opened with `new WebSocket(url)` and LIMIT / 2 - 1
    // accepted pairs.
    const firstOutgoingWebSocket = await this.#connect();
    await this.#connect();
    for (let i = 0; i < LIMIT / 2 - 2; i++) {
      this.#acceptPair();
    }
    const lastConnectedPair = this.#acceptPair();

    const extra = this.#pair();
    assert.throws(() => extra.server.accept(), { message: LIMIT_MESSAGE });

    // Hibernatable WebSockets have their own limit, so this actor can still accept one.
    this.ctx.acceptWebSocket(this.#pair().server);

    // Closing a pair frees its two slots. Each end gives its slot back once its outgoing close
    // frame has been sent, which for the client can be after the server's close event. Opening
    // a connection takes a round trip through the global outbound, by which time both have.
    const serverClosed = new Promise((resolve) => {
      lastConnectedPair.server.addEventListener('close', resolve);
    });
    lastConnectedPair.client.addEventListener('close', () =>
      lastConnectedPair.client.close()
    );
    lastConnectedPair.server.close();
    await serverClosed;
    await this.#connect();
    await this.#connect();

    // Now full again: a connection is rejected once it opens, and is closed rather than kept, even
    // when its 'error' listener throws.
    const rejected = new WebSocket('ws://example.com/');
    this.#connections.push(rejected);
    const rejection = await new Promise((resolve) => {
      rejected.addEventListener('error', (event) => {
        resolve(event.error);
        throw new Error('error listener failure');
      });
    });
    assert.strictEqual(rejection.message, LIMIT_MESSAGE);
    await this.env.server
      .get(this.env.server.idFromName('server'))
      .waitForDisconnect();
    assert.throws(() => extra.server.accept(), { message: LIMIT_MESSAGE });

    // A WebSocket whose message listener throws is torn down while its JS object is still held,
    // and gives back exactly its own slot. Its peer is in the server object, so it doesn't count.
    const failed = new Promise((resolve) => {
      firstOutgoingWebSocket.addEventListener('error', resolve);
    });
    firstOutgoingWebSocket.addEventListener('message', () => {
      throw new Error('message listener failure');
    });
    firstOutgoingWebSocket.send('ping');
    const event = await failed;
    assert.strictEqual(event.error.message, 'message listener failure');
    assert.doesNotThrow(() => this.#pair().server.accept());
    assert.throws(() => extra.server.accept(), { message: LIMIT_MESSAGE });
  }
}

// The remote end of every `new WebSocket(url)` connection the test opens, which echoes back each
// message it receives. It is a separate Durable Object, so its WebSockets count toward its own
// limit, not the one under test.
export class WebSocketServerObject extends DurableObject {
  // The server ends, held so that none is garbage-collected, which would disconnect its client.
  #open = [];
  #disconnected = Promise.withResolvers();

  fetch() {
    const [client, server] = Object.values(new WebSocketPair());
    this.#open.push(server);
    server.accept();
    server.addEventListener('message', (event) => server.send(event.data));
    server.addEventListener('close', () => this.#disconnected.resolve());
    return new Response(null, { status: 101, webSocket: client });
  }

  // Resolves once the remote end of any connection has gone away.
  async waitForDisconnect() {
    await this.#disconnected.promise;
  }
}

// The global outbound, which passes every request to the server object.
export const webSocketServer = {
  fetch(request, env) {
    return env.server.get(env.server.idFromName('server')).fetch(request);
  },
};

export const acceptLimit = {
  async test(ctrl, env) {
    const stub = env.ns.get(env.ns.idFromName('accept-limit'));
    await stub.run();
  },
};
