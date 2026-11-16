// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Messages queued on a pending port survive garbage collection, including
// one run while start() is delivering them. (In C++ the queue is a list of
// JsRefs the port traces, which start() must make strong again as it moves
// them into its untraced continuation.)
//
// Not pinned: what happens when one port of a pair is collected. Both
// implementations then close the other without a 'close' event, at a time
// that depends on the collector (divergence #4 in AGENTS.md).

import { deepStrictEqual, ok, strictEqual } from 'node:assert';

export const queuedMessagesSurviveGc = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const kCount = 8;

    const makeMessage = (i) => ({
      index: i,
      nested: { label: `message-${i}`, list: [i, i + 1, i + 2] },
      bytes: new Uint8Array([i, i, i, i]),
    });

    const checkMessage = (data, i) => {
      strictEqual(data.index, i);
      strictEqual(data.nested.label, `message-${i}`);
      deepStrictEqual(data.nested.list, [i, i + 1, i + 2]);
      ok(data.bytes instanceof Uint8Array);
      deepStrictEqual(Array.from(data.bytes), [i, i, i, i]);
    };

    const runPhase = async (base) => {
      // postMessage() clones its argument, so the queued copies are
      // reachable only through port2's queue.
      for (let i = 0; i < kCount; i++) port1.postMessage(makeMessage(base + i));

      for (let i = 0; i < 5; i++) gc();

      const received = [];
      const { promise, resolve } = Promise.withResolvers();
      const handler = (event) => {
        received.push(event.data);
        // Collect while the remaining messages are held only by the
        // delivery in progress.
        if (received.length === 1) {
          for (let i = 0; i < 5; i++) gc();
        }
        if (received.length === kCount) resolve();
      };
      // The first 'message' listener starts the port, draining the queue.
      port2.addEventListener('message', handler);
      await promise;

      strictEqual(received.length, kCount);
      received.forEach((data, i) => checkMessage(data, base + i));

      // Removing the last listener returns the port to queueing.
      port2.removeEventListener('message', handler);
    };

    // The first phase starts the port from its initial queueing state, the
    // second from one re-entered after it had been started.
    await runPhase(0);
    await runPhase(100);
    port1.close();
  },
};

// A channel's ports, and a port its listeners hold, stay usable across
// collection.
export const portsSurviveGc = {
  async test() {
    const channel = new MessageChannel();
    for (let i = 0; i < 5; i++) gc();
    const { promise, resolve } = Promise.withResolvers();
    channel.port2.onmessage = (event) => resolve(event.data);
    for (let i = 0; i < 5; i++) gc();
    channel.port1.postMessage('after gc');
    strictEqual(await promise, 'after gc');
    channel.port1.close();
  },
};
