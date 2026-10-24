// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Where structuredClone lives and the shape of the function. Divergences:
// length is 0 in C++ and 1 in TypeScript (WebIDL: the optional options
// argument does not count), ledger #1; the source text is native code only in
// C++, ledger #2.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const placement = {
  test() {
    // A method of ServiceWorkerGlobalScope.prototype, not an own property of
    // the global, and the only one on the chain.
    strictEqual(Object.hasOwn(globalThis, 'structuredClone'), false);
    const proto = Object.getPrototypeOf(globalThis);
    deepStrictEqual(Object.getOwnPropertyDescriptor(proto, 'structuredClone'), {
      value: proto.structuredClone,
      writable: true,
      enumerable: true,
      configurable: true,
    });
    for (
      let p = Object.getPrototypeOf(proto);
      p;
      p = Object.getPrototypeOf(p)
    ) {
      strictEqual(Object.hasOwn(p, 'structuredClone'), false);
    }
    strictEqual(globalThis.structuredClone, proto.structuredClone);
  },
};

export const functionShape = {
  test() {
    strictEqual(structuredClone.name, 'structuredClone');
    strictEqual(structuredClone.length, usingTsImpl ? 1 : 0);
    deepStrictEqual(Reflect.ownKeys(structuredClone), ['length', 'name']);
    deepStrictEqual(
      Object.getOwnPropertyDescriptor(structuredClone, 'length'),
      {
        value: usingTsImpl ? 1 : 0,
        writable: false,
        enumerable: false,
        configurable: true,
      }
    );
    strictEqual(Object.getPrototypeOf(structuredClone), Function.prototype);
  },
};

export const notAConstructor = {
  test() {
    throws(() => new structuredClone(1), {
      name: 'TypeError',
      message: /is not a constructor/,
    });
  },
};

export const sourceText = {
  test() {
    const source = Function.prototype.toString.call(structuredClone);
    strictEqual(
      source === 'function structuredClone() { [native code] }',
      !usingTsImpl
    );
    strictEqual(source.startsWith('structuredClone('), usingTsImpl);
  },
};

export const replaceableAndDeletable = {
  test() {
    const proto = Object.getPrototypeOf(globalThis);
    const original = proto.structuredClone;
    try {
      globalThis.structuredClone = 'shadowed';
      strictEqual(Object.hasOwn(globalThis, 'structuredClone'), true);
      strictEqual(proto.structuredClone, original);
      delete globalThis.structuredClone;
      strictEqual(globalThis.structuredClone, original);
      delete proto.structuredClone;
      strictEqual('structuredClone' in globalThis, false);
    } finally {
      delete globalThis.structuredClone;
      Object.defineProperty(proto, 'structuredClone', {
        value: original,
        writable: true,
        enumerable: true,
        configurable: true,
      });
    }
  },
};
