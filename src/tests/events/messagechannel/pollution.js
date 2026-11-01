// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Delivery and listener bookkeeping use nothing user code can patch: not
// Promise.prototype.then or .constructor (the species lookup), not
// Array.prototype.push, not WeakRef.prototype.deref. The test itself must
// not await while the patches are in place (await looks up a promise's
// constructor), so it returns its promise instead, and the patches are
// lifted in the listener that receives the last message.

import { deepStrictEqual } from 'node:assert';

export const deliveryIgnoresPollutedPrototypes = {
  test() {
    const { port1, port2 } = new MessageChannel();
    // Recorded by index: push is one of the patched methods.
    const patched = [];
    const record = (name) => {
      patched[patched.length] = name;
    };
    // WeakRef is a global only at some compat dates; the TypeScript
    // implementation captured its own before user code ran.
    const patchWeakRef = typeof WeakRef !== 'undefined';
    const saved = {
      then: Object.getOwnPropertyDescriptor(Promise.prototype, 'then'),
      constructor: Object.getOwnPropertyDescriptor(
        Promise.prototype,
        'constructor'
      ),
      push: Object.getOwnPropertyDescriptor(Array.prototype, 'push'),
      deref: patchWeakRef
        ? Object.getOwnPropertyDescriptor(WeakRef.prototype, 'deref')
        : undefined,
    };
    const restore = () => {
      Object.defineProperty(Promise.prototype, 'then', saved.then);
      Object.defineProperty(
        Promise.prototype,
        'constructor',
        saved.constructor
      );
      Object.defineProperty(Array.prototype, 'push', saved.push);
      if (patchWeakRef) {
        Object.defineProperty(WeakRef.prototype, 'deref', saved.deref);
      }
    };

    const received = [];
    const { promise, resolve, reject } = Promise.withResolvers();
    const checked = promise.then(() => {
      deepStrictEqual(received, [1, 2, 3]);
      deepStrictEqual(patched, []);
      port1.close();
    });

    const realThen = saved.then.value;
    const realPush = saved.push.value;
    Object.defineProperty(Promise.prototype, 'then', {
      configurable: true,
      writable: true,
      value: function (...args) {
        record('then');
        return realThen.apply(this, args);
      },
    });
    Object.defineProperty(Promise.prototype, 'constructor', {
      configurable: true,
      get() {
        record('constructor');
        return Promise;
      },
    });
    Object.defineProperty(Array.prototype, 'push', {
      configurable: true,
      writable: true,
      value: function (...args) {
        record('push');
        return realPush.apply(this, args);
      },
    });
    if (patchWeakRef) {
      Object.defineProperty(WeakRef.prototype, 'deref', {
        configurable: true,
        writable: true,
        value() {
          record('deref');
          return undefined;
        },
      });
    }

    try {
      // Queued while there is no listener, then delivered once there is.
      port1.postMessage(1);
      port1.postMessage(2);
      port2.addEventListener('message', (event) => {
        received[received.length] = event.data;
        if (received.length === 3) {
          restore();
          resolve();
        }
      });
      port1.postMessage(3);
    } catch (e) {
      restore();
      reject(e);
    }
    return checked;
  },
};
