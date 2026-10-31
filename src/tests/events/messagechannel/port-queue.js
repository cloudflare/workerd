// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The port message queue follows the port's 'message' listeners: the first
// one, whether assigned to onmessage or added with addEventListener(),
// starts delivery, and removing the last returns the port to queueing.
// (Node.js behavior; per spec only onmessage or start() enables the queue.)

import { strictEqual } from 'node:assert';
import { mock } from 'node:test';

export const addEventListenerStartsPort = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const { promise, resolve } = Promise.withResolvers();
    port2.addEventListener('message', (event) => resolve(event.data));
    port1.postMessage('hello');
    strictEqual(await promise, 'hello');
    port1.close();
  },
};

// start() starts delivery without a listener: the queued message goes to
// nobody, and a listener added afterwards sees only later messages.
export const startWithoutListenersConsumesTheQueue = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    port1.postMessage('queued');
    const received = mock.fn();
    // A listener for another type does not start the port.
    port2.addEventListener('messageerror', received);
    await scheduler.wait(10);
    strictEqual(received.mock.callCount(), 0);

    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = null;
    port2.start();
    port2.start();
    await scheduler.wait(10);
    port2.addEventListener('message', (event) => resolve(event.data));
    port1.postMessage('later');
    strictEqual(await promise, 'later');
    port1.close();
  },
};

export const removingLastListenerRequeues = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const first = Promise.withResolvers();
    const handler = (event) => first.resolve(event.data);
    port2.addEventListener('message', handler);
    port1.postMessage('one');
    strictEqual(await first.promise, 'one');

    port2.removeEventListener('message', handler);
    port1.postMessage('two');
    await scheduler.wait(10);

    const second = Promise.withResolvers();
    port2.addEventListener('message', (event) => second.resolve(event.data));
    strictEqual(await second.promise, 'two');
    port1.close();
  },
};

// A once-listener starts the port; its removal after the first message
// returns the port to pending, so later messages queue.
export const onceListenerReturnsPortToPending = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const first = Promise.withResolvers();
    port2.addEventListener('message', (event) => first.resolve(event.data), {
      once: true,
    });
    port1.postMessage('one');
    strictEqual(await first.promise, 'one');

    port1.postMessage('two');
    await scheduler.wait(10);

    const second = Promise.withResolvers();
    port2.addEventListener('message', (event) => second.resolve(event.data));
    strictEqual(await second.promise, 'two');
    port1.close();
  },
};

// A listener removed by its AbortSignal counts as removed.
export const abortedListenerReturnsPortToPending = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const controller = new AbortController();
    const first = Promise.withResolvers();
    port2.addEventListener('message', (event) => first.resolve(event.data), {
      signal: controller.signal,
    });
    port1.postMessage('one');
    strictEqual(await first.promise, 'one');
    controller.abort();

    port1.postMessage('two');
    await scheduler.wait(10);

    const second = Promise.withResolvers();
    port2.addEventListener('message', (event) => second.resolve(event.data));
    strictEqual(await second.promise, 'two');
    port1.close();
  },
};

// A closed port is terminal: attaching listeners or manipulating onmessage
// afterwards never restarts it, and nothing is delivered.
export const closedPortIsTerminal = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    port1.postMessage('queued');
    port2.close();

    const handler = mock.fn();
    port2.onmessage = null;
    port2.onmessage = handler;
    port2.addEventListener('message', handler);
    port2.start();
    port1.postMessage('late');
    await scheduler.wait(10);
    strictEqual(handler.mock.callCount(), 0);
  },
};

// Messages posted on either side of a closed pair are dropped.
export const postingToAClosedPairDrops = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    port2.close();
    const handler = mock.fn();
    port2.onmessage = handler;
    port1.onmessage = handler;
    port1.postMessage('nope');
    port2.postMessage('nope');
    await scheduler.wait(10);
    strictEqual(handler.mock.callCount(), 0);
  },
};
