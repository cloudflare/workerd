// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// postMessage() structured-clones its message before routing it, so what
// cannot be cloned throws whether or not anything would receive it, and the
// receiver gets a copy. Selected from WPT webmessaging
// (Channel_postMessage_Blob.any.js, Channel_postMessage_DataCloneErr.any.js).

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { mock } from 'node:test';

export const sharedArrayBuffersAreRejected = {
  test() {
    const { port1 } = new MessageChannel();
    for (const message of [
      new SharedArrayBuffer(1),
      { nested: [new SharedArrayBuffer(1)] },
    ]) {
      throws(() => port1.postMessage(message), {
        name: 'TypeError',
        message: 'SharedArrayBuffer is unsupported with MessagePort',
      });
    }
  },
};

export const uncloneableValuesThrow = {
  async test() {
    const { port1 } = new MessageChannel();
    throws(() => port1.postMessage(() => {}), {
      name: 'DataCloneError',
      message: '() => {} could not be cloned.',
    });
    throws(() => port1.postMessage(Symbol()), {
      name: 'DataCloneError',
      message: 'Symbol() could not be cloned.',
    });
    const { RpcTarget } = await import('cloudflare:workers');
    class Foo extends RpcTarget {}
    throws(() => port1.postMessage(new Foo()), {
      code: 25, // DATA_CLONE_ERR
      name: 'DataCloneError',
    });
  },
};

// A closed pair still serializes: getters still run, and an uncloneable
// value still throws.
export const closedPairStillSerializes = {
  test() {
    const { port1, port2 } = new MessageChannel();
    port2.close();
    let read = false;
    port1.postMessage({
      get value() {
        read = true;
        return 1;
      },
    });
    ok(read);
    throws(() => port1.postMessage(() => {}), { name: 'DataCloneError' });
  },
};

// The peer is looked for after serialization, so a port closed by a getter
// during it does not deliver the message.
export const closingDuringSerializationDropsTheMessage = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const received = mock.fn();
    port2.onmessage = received;
    port1.postMessage({
      get value() {
        port1.close();
        return 1;
      },
    });
    await scheduler.wait(10);
    strictEqual(received.mock.callCount(), 0);
  },
};

export const receiverGetsACopy = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const data = { a: [1, 2], b: new Map([[1, 2]]), c: new Uint8Array([3]) };
    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = (event) => resolve(event.data);
    port1.postMessage(data);
    const received = await promise;
    deepStrictEqual(received, data);
    ok(received !== data);
    port1.close();
  },
};

// Per the spec, Blob is serializable, so it round-trips.
export const blobsRoundTrip = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const { promise, resolve } = Promise.withResolvers();
    port2.onmessage = (event) => resolve(event.data);
    port1.postMessage(new Blob(['hello'], { type: 'text/plain' }));
    const received = await promise;
    ok(received instanceof Blob);
    strictEqual(received.type, 'text/plain');
    strictEqual(await received.text(), 'hello');
    port1.close();
  },
};
