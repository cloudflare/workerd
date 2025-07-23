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
  test() {
    // Per the spec, Blob is a serializable object, but we don't implement it as such currently.
    // So attempting to post a blob should throw an error.
    const { port1 } = new MessageChannel();
    throws(() => port1.postMessage(new Blob([''])), {
      code: 25, // DATA_CLONE_ERR,
      name: 'DataCloneError',
      message: /Could not serialize/,
    });
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
