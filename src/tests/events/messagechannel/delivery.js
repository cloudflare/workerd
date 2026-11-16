// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What is posted to one port is delivered to the other, in either
// direction, as a trusted MessageEvent whose source is the receiving port.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { mock } from 'node:test';

export const postedBeforeListening = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    port1.postMessage(1);
    port2.postMessage(1);
    const { promise, resolve } = Promise.withResolvers();
    let calls = 0;
    const handler = mock.fn((event) => {
      strictEqual(event.data, 1);
      if (++calls === 2) resolve();
    });
    port2.onmessage = handler;
    port1.onmessage = handler;
    await promise;
    strictEqual(handler.mock.callCount(), 2);
    port1.close();
  },
};

export const postedWhileListening = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const { promise, resolve } = Promise.withResolvers();
    let calls = 0;
    const handler = mock.fn(() => {
      if (++calls === 2) resolve();
    });
    port2.onmessage = handler;
    port1.onmessage = handler;
    port1.postMessage(1);
    port2.postMessage(1);
    await promise;
    strictEqual(handler.mock.callCount(), 2);
    port1.close();
  },
};

export const deliveredEventShape = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = resolve;
    port1.postMessage('hi');
    const event = await promise;
    ok(event instanceof MessageEvent);
    strictEqual(event.type, 'message');
    strictEqual(event.isTrusted, true);
    strictEqual(event.bubbles, false);
    strictEqual(event.cancelable, false);
    strictEqual(event.data, 'hi');
    strictEqual(event.origin, null);
    strictEqual(event.lastEventId, '');
    strictEqual(event.source, port2);
    strictEqual(event.target, port2);
    deepStrictEqual(event.ports, []);
    port1.close();
  },
};

// Delivery is deferred: nothing is dispatched inside postMessage().
export const deliveryIsDeferred = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const order = [];
    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = (event) => {
      order.push(event.data);
      if (event.data === 2) resolve();
    };
    await scheduler.wait(1);
    port1.postMessage(1);
    order.push('posted');
    port1.postMessage(2);
    await promise;
    deepStrictEqual(order, ['posted', 1, 2]);
    port1.close();
  },
};
