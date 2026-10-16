// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The WebIDL shape of EventTarget: member placement, property attributes
// and function metadata, and the brand checks.
//
// Divergences (ledger in AGENTS.md):
// - #15: C++ methods report length 0.
// - #16: C++ Symbol.toStringTag is writable.
// - #17: C++ EventTarget's prototype property is writable.
// - #18: C++ EventTarget.prototype carries the cloudflare:internal-class
//   marker.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const kMethods = ['addEventListener', 'removeEventListener', 'dispatchEvent'];

// #16.
export const toStringTagBranding = {
  test() {
    deepStrictEqual(
      Object.getOwnPropertyDescriptor(
        EventTarget.prototype,
        Symbol.toStringTag
      ),
      {
        value: 'EventTarget',
        writable: !usingTsImpl,
        enumerable: false,
        configurable: true,
      }
    );
    strictEqual(
      Object.prototype.toString.call(new EventTarget()),
      '[object EventTarget]'
    );
  },
};

export const methodsArePrototypeMethods = {
  test() {
    for (const name of kMethods) {
      const { value, writable, enumerable, configurable } =
        Object.getOwnPropertyDescriptor(EventTarget.prototype, name);
      strictEqual(typeof value, 'function', name);
      strictEqual(value.name, name);
      strictEqual(writable, true, name);
      strictEqual(enumerable, true, name);
      strictEqual(configurable, true, name);
    }
    deepStrictEqual(Reflect.ownKeys(new EventTarget()), []);
  },
};

// #15.
export const functionMetadata = {
  test() {
    strictEqual(EventTarget.name, 'EventTarget');
    strictEqual(EventTarget.length, 0);
    deepStrictEqual(
      kMethods.map((name) => EventTarget.prototype[name].length),
      usingTsImpl ? [2, 2, 1] : [0, 0, 0]
    );
  },
};

// #17.
export const interfaceObject = {
  test() {
    strictEqual(Object.getPrototypeOf(EventTarget), Function.prototype);
    strictEqual(Object.getPrototypeOf(EventTarget.prototype), Object.prototype);
    deepStrictEqual(Object.getOwnPropertyDescriptor(EventTarget, 'prototype'), {
      value: EventTarget.prototype,
      writable: !usingTsImpl,
      enumerable: false,
      configurable: false,
    });
    deepStrictEqual(
      Object.getOwnPropertyDescriptor(EventTarget.prototype, 'constructor'),
      {
        value: EventTarget,
        writable: true,
        enumerable: false,
        configurable: true,
      }
    );
    throws(() => EventTarget(), TypeError);
  },
};

// Every method rejects a receiver that is not an EventTarget, an object that
// merely inherits from EventTarget.prototype included.
export const brandChecks = {
  test() {
    const listener = () => {};
    for (const receiver of [
      {},
      undefined,
      null,
      new Event('x'),
      Object.create(EventTarget.prototype),
    ]) {
      const { addEventListener, removeEventListener, dispatchEvent } =
        EventTarget.prototype;
      throws(() => addEventListener.call(receiver, 'x', listener), TypeError);
      throws(
        () => removeEventListener.call(receiver, 'x', listener),
        TypeError
      );
      throws(() => dispatchEvent.call(receiver, new Event('x')), TypeError);
    }
  },
};

// Neither implementation substitutes the global for a missing receiver.
// (The global's own methods are other functions: interop.js.)
export const detachedMethodsThrow = {
  test() {
    const { addEventListener, removeEventListener, dispatchEvent } =
      EventTarget.prototype;
    throws(() => addEventListener('x', () => {}), TypeError);
    throws(() => removeEventListener('x', () => {}), TypeError);
    throws(() => dispatchEvent(new Event('x')), TypeError);
  },
};

// #18.
export const internalClassMarker = {
  test() {
    const marker = Symbol.for('cloudflare:internal-class');
    strictEqual(
      EventTarget.prototype[marker],
      usingTsImpl ? undefined : marker
    );
  },
};
