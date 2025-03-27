import { ok, strictEqual, throws } from 'node:assert';

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
    port1.close();
    port2.onmessage = () => {
      throw new Error('should not be called');
    };
    port1.postMessage('nope');
    await scheduler.wait(10);
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
