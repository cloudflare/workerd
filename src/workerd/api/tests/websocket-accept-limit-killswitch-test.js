// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';

const LIMIT = 32768;

export class WebSocketLimitObject extends DurableObject {
  // Every pair the test creates, held so that none is garbage-collected.
  #pairs = [];

  // With the limit's kill switch on, this actor can accept more WebSockets than the limit.
  run() {
    for (let i = 0; i < LIMIT / 2 + 1; i++) {
      const [client, server] = Object.values(new WebSocketPair());
      client.accept();
      server.accept();
      this.#pairs.push({ client, server });
    }
  }
}

export const acceptPastLimit = {
  async test(ctrl, env) {
    const stub = env.ns.get(env.ns.idFromName('accept-past-limit'));
    await stub.run();
  },
};
