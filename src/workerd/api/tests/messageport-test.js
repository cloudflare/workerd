import { ok, strictEqual, throws } from 'node:assert';

import { mock } from 'node:test';

export const simple1 = {
  test() {
    const { port1, port2 } = new MessageChannel();
    ok(port1 instanceof MessagePort);
    ok(port2 instanceof MessagePort);
    port1.postMessage(1);
    port2.postMessage(1);
    const handler = mock.fn((event) => {
      strictEqual(event.data, 1);
    });
    port2.onmessage = handler;
    port1.onmessage = handler;
    strictEqual(handler.mock.callCount(), 2);
  },
};

export const simple2 = {
  test() {
    const { port1, port2 } = new MessageChannel();
    ok(port1 instanceof MessagePort);
    ok(port2 instanceof MessagePort);
    const handler = mock.fn((event) => {
      strictEqual(event.data, 1);
    });
    port2.onmessage = handler;
    port1.onmessage = handler;
    port1.postMessage(1);
    port2.postMessage(1);
    strictEqual(handler.mock.callCount(), 2);
  },
};

export const simple3 = {
  test() {
    const { port1, port2 } = new MessageChannel();
    port1.close();
    port2.onmessage = () => {
      throw new Error('should not be called');
    };
    port1.postMessage('nope');
  },
};

export const simple4 = {
  test() {
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
  },
};

export const simple5 = {
  test() {
    const { port1 } = new MessageChannel();
    throws(() => port1.postMessage(1, [1]), {
      message: 'Transfer list is not supported',
    });
    throws(() => port1.postMessage(1, { transfer: [1] }), {
      message: 'Transfer list is not supported',
    });
    // If the lists are empty it is ok.
    port1.postMessage(1, []);
    port1.postMessage(1, { transfer: [] });
  },
};

export const simple6 = {
  test() {
    const { port1, port2 } = new MessageChannel();
    // Errors occurring when dispatching the event should be passed
    // to globalThis.reportError(...).
    port2.onmessage = () => {
      throw new Error('boom');
    };
    const handler = mock.fn((event) => {
      strictEqual(event.error.message, 'boom');
    });
    addEventListener('error', handler, { once: true });
    port1.postMessage('test');
    strictEqual(handler.mock.callCount(), 1);
  },
};

export const simple7 = {
  test() {
    const { port1, port2 } = new MessageChannel();
    // Errors occurring when dispatching the event should be passed
    // to globalThis.reportError(...). In this case, the error handler
    // needs to be registered before the onmessage property is set
    // since setting the onmessage will trigger the pending message
    // to be delivered synchronously.
    port1.postMessage('test');
    const handler = mock.fn((event) => {
      strictEqual(event.error.message, 'boom');
    });
    addEventListener('error', handler, { once: true });
    port2.onmessage = () => {
      throw new Error('boom');
    };
    strictEqual(handler.mock.callCount(), 1);
  },
};

export default {
  rpc(port) {
    port.onmessage = (event) => {
      strictEqual(event.data, 'hello');
      // TODO(message-port): The test fails if we post the message here.
      // Figure out why...
      // port.postMessage('world');
    };
    port.postMessage('world');
  },
};

export const rpcTest = {
  async test(_, env, ctx) {
    const { port1, port2 } = new MessageChannel();
    const { promise, resolve } = Promise.withResolvers();
    port1.onmessage = (event) => {
      strictEqual(event.data, 'world');
      resolve();
    };
    port1.postMessage('hello');
    await env.FOO.rpc(port2);
    await promise;
  },
};
