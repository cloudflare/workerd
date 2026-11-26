// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// close() on a socket upgraded with startTls() must close the connection under it, so that the
// other end's reads end. startTls() hands the connection to the socket it returns, and the
// plain-text socket it leaves behind must not keep the connection open.
//
// The connection is a tunnel to a Durable Object's connect() handler, which tells the test through
// RPC how its read ended.

import { DurableObject } from 'cloudflare:workers';
import { strictEqual } from 'node:assert';

export class Peer extends DurableObject {
  #readEnded = Promise.withResolvers();

  async connect(socket) {
    const secure = socket.startTls();
    await secure.opened;

    // The test sends nothing, so this read ends only when the connection does.
    const reader = secure.readable.getReader();
    try {
      const { done } = await reader.read();
      this.#readEnded.resolve(done ? 'EOF' : 'data');
    } catch (e) {
      this.#readEnded.resolve(e.message);
    }
  }

  readEnded() {
    return this.#readEnded.promise;
  }
}

export const closingUpgradedSocketClosesConnection = {
  async test(ctrl, env) {
    const stub = env.PEER.get(env.PEER.idFromName('peer'));
    const socket = stub.connect('smtp.example.com:25', {
      secureTransport: 'starttls',
    });
    await socket.opened;
    const secure = socket.startTls();
    await secure.opened;

    await secure.close();
    strictEqual(
      await stub.readEnded(),
      "Can't read from request stream because client disconnected."
    );
  },
};
