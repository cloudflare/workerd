// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Built-ins that user code patches after startup do not reach the
// conversion, other than the iterator the transfer member is read through
// (see transfer-conversion.js).

import { deepStrictEqual, strictEqual } from 'node:assert';

// Captured so the helper still works while Object.defineProperty is patched.
const { defineProperty, getOwnPropertyDescriptor } = Reflect;

function withPatched(object, key, value, fn) {
  const saved = getOwnPropertyDescriptor(object, key);
  defineProperty(object, key, {
    value,
    writable: true,
    configurable: true,
  });
  try {
    fn();
  } finally {
    defineProperty(object, key, saved);
  }
}

export const patchedBuiltins = {
  test() {
    const boom = () => {
      throw new Error('patched built-in was called');
    };
    const buffer = new ArrayBuffer(4);
    let clone;
    withPatched(Array.prototype, 'push', boom, () => {
      withPatched(Object, 'defineProperty', boom, () => {
        withPatched(globalThis, 'TypeError', boom, () => {
          clone = structuredClone(buffer, { transfer: [buffer] });
        });
      });
    });
    deepStrictEqual([buffer.byteLength, clone.byteLength], [0, 4]);
  },
};

export const errorsUseTheRealTypeError = {
  test() {
    const RealTypeError = TypeError;
    let error;
    withPatched(
      globalThis,
      'TypeError',
      function NotTypeError() {},
      () => {
        try {
          structuredClone(1, 1);
        } catch (e) {
          error = e;
        }
      }
    );
    strictEqual(Object.getPrototypeOf(error), RealTypeError.prototype);
  },
};
