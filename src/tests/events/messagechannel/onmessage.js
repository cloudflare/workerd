// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// onmessage is an HTML event handler attribute: its handler takes a
// position in the listener list when first assigned and keeps it across
// reassignment.

import { deepStrictEqual, strictEqual } from 'node:assert';

export const onmessageDefaultsToNull = {
  test() {
    const { port1 } = new MessageChannel();
    strictEqual(port1.onmessage, null);
  },
};

export const onmessagePositionalOrdering = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const order = [];
    const { promise, resolve } = Promise.withResolvers();

    port2.addEventListener('message', () => order.push('a'));
    const b1 = () => order.push('b1');
    port2.onmessage = b1;
    strictEqual(port2.onmessage, b1);
    port2.addEventListener('message', () => order.push('c'));

    // Reassignment keeps the original position.
    port2.onmessage = () => order.push('b2');

    port2.addEventListener('message', () => resolve());
    port1.postMessage('hello');
    await promise;
    deepStrictEqual(order, ['a', 'b2', 'c']);
    port1.close();
  },
};

// Clearing onmessage and assigning it again takes a fresh position at the
// end of the listener list.
export const onmessageClearedTakesFreshPosition = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const order = [];
    const { promise, resolve } = Promise.withResolvers();

    port2.onmessage = () => order.push('handler1');
    port2.addEventListener('message', () => order.push('listener'));

    port2.onmessage = null;
    strictEqual(port2.onmessage, null);
    port2.onmessage = () => order.push('handler2');

    port2.addEventListener('message', () => resolve());
    port1.postMessage('hello');
    await promise;
    deepStrictEqual(order, ['listener', 'handler2']);
    port1.close();
  },
};

// A non-callable object is retained as the attribute value and enables the
// port's message queue, but is never invoked: messages delivered while it
// is assigned are consumed and dropped.
export const onmessageNonCallableStartsPort = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const obj = {};
    port2.onmessage = obj;
    strictEqual(port2.onmessage, obj);
    port1.postMessage('lost');
    await scheduler.wait(10);

    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = (event) => resolve(event.data);
    port1.postMessage('kept');
    strictEqual(await promise, 'kept');
    port1.close();
  },
};

// A primitive deactivates the handler, and reads back as null.
export const onmessagePrimitiveClears = {
  test() {
    const { port1 } = new MessageChannel();
    port1.onmessage = () => {};
    port1.onmessage = 5;
    strictEqual(port1.onmessage, null);
    port1.close();
  },
};

// The handler is invoked with the global object as `this`, as JSG invokes
// one assigned through a C++ setter.
export const onmessageHandlerReceiver = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = function () {
      resolve(this);
    };
    port1.postMessage(1);
    strictEqual(await promise, globalThis);
    port1.close();
  },
};
