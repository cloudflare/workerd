// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';

import { mock } from 'node:test';

export const simple1 = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    ok(port1 instanceof MessagePort);
    ok(port2 instanceof MessagePort);
    port1.postMessage(1);
    port2.postMessage(1);
    const { promise, resolve } = Promise.withResolvers();
    const handler = mock.fn((event) => {
      strictEqual(event.data, 1);
      strictEqual(event.isTrusted, true);
      resolve();
    });
    port2.onmessage = handler;
    port1.onmessage = handler;
    await promise;
    strictEqual(handler.mock.callCount(), 2);
  },
};

export const simple2 = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    ok(port1 instanceof MessagePort);
    ok(port2 instanceof MessagePort);
    const { promise, resolve } = Promise.withResolvers();
    const handler = mock.fn((event) => {
      strictEqual(event.data, 1);
      resolve();
    });
    port2.onmessage = handler;
    port1.onmessage = handler;
    port1.postMessage(1);
    port2.postMessage(1);
    await promise;
    strictEqual(handler.mock.callCount(), 2);
  },
};

export const simple3 = {
  async test() {
    const { port1, port2 } = new MessageChannel();

    const closeHandler = mock.fn();
    port1.onclose = closeHandler;
    port2.onclose = closeHandler;

    port1.close();
    port2.onmessage = () => {
      throw new Error('should not be called');
    };
    port1.postMessage('nope');
    await scheduler.wait(10);
    strictEqual(closeHandler.mock.callCount(), 2);
  },
};

export const simple4 = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    port2.close();
    port2.onmessage = () => {
      throw new Error('should not be called');
    };
    port1.onmessage = () => {
      throw new Error('should not be called');
    };
    port1.postMessage('nope');
    port2.postMessage('nope');
    await scheduler.wait(10);
  },
};

export const simple5 = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    throws(() => port1.postMessage(1, [1]), {
      message: 'Transfer list is not supported',
    });
    throws(() => port1.postMessage(1, { transfer: [1] }), {
      message: 'Transfer list is not supported',
    });
    // If the lists are empty it is ok.
    port1.postMessage(1, []);
    port1.postMessage(1, { transfer: [] });

    const handler = mock.fn((event) => {
      strictEqual(event.data, 1);
    });
    port2.onmessage = handler;
    await scheduler.wait(10);
    strictEqual(handler.mock.callCount(), 2);
  },
};

// The following are a selected subset of web platform tests for MessageChannel and MessagePort
// that we know we pass. We don't support the full MessagePort spec so we're not going to run
// the full WPT's for these yet.
// Refs: https://github.com/web-platform-tests/wpt/blob/master/webmessaging/Channel_postMessage_Blob.any.js
// Refs: https://github.com/web-platform-tests/wpt/blob/master/webmessaging/Channel_postMessage_DataCloneErr.any.js

export const postMessageBlob = {
  async test() {
    // Per the spec, Blob is a serializable object, so it round-trips through postMessage.
    const { port1, port2 } = new MessageChannel();
    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = (event) => resolve(event.data);
    port1.postMessage(new Blob(['hello'], { type: 'text/plain' }));
    const received = await promise;
    ok(received instanceof Blob);
    strictEqual(received.type, 'text/plain');
    strictEqual(await received.text(), 'hello');
  },
};

export const postMessageRpcTarget = {
  async test() {
    const { RpcTarget } = await import('cloudflare:workers');
    class Foo extends RpcTarget {}

    const { port1 } = new MessageChannel();
    throws(() => port1.postMessage(new Foo()), {
      code: 25, // DATA_CLONE_ERR,
      name: 'DataCloneError',
    });
  },
};

// Subset of the Web Platform Tests we know we don't pass, listed for future reference:
// Most the web messaging WPT's are set up to require a full implementation of MessagePort
// with web workers and most of the tests are in html files. We'll come back to these
// later.
// * https://github.com/web-platform-tests/wpt/blob/master/webmessaging/Channel_postMessage_clone_port.any.js
// * https://github.com/web-platform-tests/wpt/blob/master/webmessaging/Channel_postMessage_clone_port_error.any.js
// * https://github.com/web-platform-tests/wpt/blob/master/webmessaging/Channel_postMessage_ports_readonly_array.any.js
// * https://github.com/web-platform-tests/wpt/blob/master/webmessaging/Channel_postMessage_transfer_xsite_incoming_messages.window.js
// * https://github.com/web-platform-tests/wpt/blob/master/webmessaging/Channel_postMessage_transfer_xsite_incoming_messages.window.js
// * https://github.com/web-platform-tests/wpt/blob/master/webmessaging/Channel_postMessage_with_transfer_incoming_messages.any.js
// * https://github.com/web-platform-tests/wpt/blob/master/webmessaging/Channel_postMessage_with_transfer_outgoing_messages.any.js

// This one is a bit special, per the spec we're supposed to fire off the close event
// on an entangled port when the other port is garbage collected. We don't do that yet
// and we might not ever. Need to investigate this further but it's not blocking us
// right now.
// * https://github.com/web-platform-tests/wpt/blob/master/webmessaging/message-channels/close-event/garbage-collected.tentative.any.js

// The onmessage handler occupies a normal position in the listener list based on when it
// was first assigned, per HTML's event handler semantics.
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
  },
};

// Clearing onmessage and assigning it again takes a fresh position at the end of the
// listener list.
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
  },
};

// Assigning a non-callable object to onmessage retains it as the attribute value and
// enables the port's message queue, but the object is never invoked: messages delivered
// while it is assigned are consumed and dropped.
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
  },
};

// Adding a 'message' listener via addEventListener starts the port, the same as assigning
// onmessage (Node.js behavior; per spec only the onmessage attribute enables the queue).
export const addEventListenerStartsPort = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const { promise, resolve } = Promise.withResolvers();
    port2.addEventListener('message', (event) => resolve(event.data));
    port1.postMessage('hello');
    strictEqual(await promise, 'hello');
  },
};

// Removing the last 'message' listener returns the port to the pending state: messages
// queue (rather than being dropped) until another listener is attached.
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
  },
};

// A once-listener starts the port; its removal after the first message returns the port
// to pending, so later messages queue until a new listener arrives.
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
  },
};

// A closed port is terminal: attaching listeners or manipulating onmessage afterwards
// never restarts it, and no messages are delivered.
export const closedPortIsTerminal = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    port1.postMessage('queued');
    port2.close();

    const handler = mock.fn();
    port2.onmessage = null;
    port2.onmessage = handler;
    port2.addEventListener('message', handler);
    port1.postMessage('late');
    await scheduler.wait(10);
    strictEqual(handler.mock.callCount(), 0);
  },
};

// A throwing 'message' listener has its exception reported (and the remaining listeners
// still run), and the port additionally dispatches a 'messageerror' event carrying the
// exception. The port itself keeps working.
export const throwingMessageListener = {
  async test() {
    const order = [];
    const boom = new Error('port boom');
    const { port1, port2 } = new MessageChannel();
    const globalHandler = () => {
      order.push('global-error');
      // Injecting a message mid-report cannot jump the queue: delivery is always deferred
      // to a later microtask, so it arrives after the current event's remaining listeners,
      // after the synthesized messageerror, and after any messages queued before it.
      port1.postMessage('injected');
    };
    globalThis.addEventListener('error', globalHandler);
    try {
      const done = Promise.withResolvers();
      port2.addEventListener('message', (event) => {
        order.push(`l1:${event.data}`);
        if (event.data === 'bad') throw boom;
        if (event.data === 'injected') done.resolve();
      });
      port2.addEventListener('message', (event) =>
        order.push(`l2:${event.data}`)
      );
      port2.addEventListener('messageerror', (event) => {
        order.push('messageerror');
        strictEqual(event.data, boom);
      });

      port1.postMessage('bad');
      port1.postMessage('after');
      await done.promise;
      deepStrictEqual(order, [
        'l1:bad',
        'global-error',
        'l2:bad',
        'messageerror',
        'l1:after',
        'l2:after',
        'l1:injected',
        'l2:injected',
      ]);
    } finally {
      globalThis.removeEventListener('error', globalHandler);
    }
  },
};

// User-constructed MessageEvents reflect the source and ports passed in their init.
// (The runtime itself never attaches either: ports are not transferable here.)
export const messageEventSourceAndPorts = {
  test() {
    const { port1, port2 } = new MessageChannel();
    const event = new MessageEvent('message', {
      data: 'x',
      source: port1,
      ports: [port1, port2],
    });
    strictEqual(event.source, port1);
    const ports = event.ports;
    strictEqual(ports.length, 2);
    strictEqual(ports[0], port1);
    strictEqual(ports[1], port2);

    // Runtime-delivered message events carry the entangled port as source and no ports.
    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = (e) => resolve(e);
    port1.postMessage('hi');
    return promise.then((e) => {
      strictEqual(e.source, port2);
      strictEqual(e.ports.length, 0);
      strictEqual(e.origin, null);
    });
  },
};
