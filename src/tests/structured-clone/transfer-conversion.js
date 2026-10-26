// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Conversion of the transfer member. C++ converts it as an Array, read by
// index; TypeScript as a WebIDL sequence<object>, read through its iterator.
// Divergences: which values are accepted (ledger #3), the messages for a
// rejected member or element (#4, #5), and whether an array's iterator runs
// (#6).

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const CPP_BAD_TRANSFER =
  "Incorrect type for the 'transfer' field on 'StructuredCloneOptions': " +
  "the provided value is not of type 'Array'.";
const TS_BAD_TRANSFER =
  "Incorrect type for the 'transfer' field on 'StructuredCloneOptions': " +
  "the provided value is not of type 'sequence<object>'.";
const CPP_BAD_ITEM = 'Object is not transferable';
const TS_BAD_ITEM =
  "Incorrect type for an element of the 'transfer' field on " +
  "'StructuredCloneOptions': the provided value is not of type 'object'.";

function transferred(transfer, buffer) {
  const clone = structuredClone(buffer, { transfer });
  return [buffer.byteLength, clone.byteLength];
}

export const arrayTransfers = {
  test() {
    const buffer = new ArrayBuffer(4);
    deepStrictEqual(transferred([buffer], buffer), [0, 4]);
  },
};

export const nonObjectMemberRejected = {
  test() {
    for (const transfer of [null, 1, 'ab', true, Symbol('s')]) {
      throws(() => structuredClone(1, { transfer }), {
        name: 'TypeError',
        message: usingTsImpl ? TS_BAD_TRANSFER : CPP_BAD_TRANSFER,
      });
    }
  },
};

export const nonIterableObjectRejected = {
  test() {
    const buffer = new ArrayBuffer(4);
    for (const transfer of [{}, { length: 1, 0: buffer }, () => {}]) {
      throws(() => structuredClone(buffer, { transfer }), {
        name: 'TypeError',
        message: usingTsImpl ? TS_BAD_TRANSFER : CPP_BAD_TRANSFER,
      });
      strictEqual(buffer.byteLength, 4);
    }
  },
};

export const iterablesOtherThanArrays = {
  test() {
    const makers = [
      (b) => new Set([b]),
      (b) =>
        (function* () {
          yield b;
        })(),
      (b) => new Proxy([b], {}),
    ];
    for (const make of makers) {
      const buffer = new ArrayBuffer(4);
      if (usingTsImpl) {
        deepStrictEqual(transferred(make(buffer), buffer), [0, 4]);
      } else {
        throws(() => transferred(make(buffer), buffer), {
          name: 'TypeError',
          message: CPP_BAD_TRANSFER,
        });
        strictEqual(buffer.byteLength, 4);
      }
    }
  },
};

export const nonObjectElementsRejected = {
  test() {
    // eslint-disable-next-line no-sparse-arrays
    for (const transfer of [[1], [null], [undefined], ['s'], [, 1]]) {
      throws(() => structuredClone(1, { transfer }), {
        name: 'TypeError',
        message: usingTsImpl ? TS_BAD_ITEM : CPP_BAD_ITEM,
      });
    }
  },
};

export const nonTransferableObjectsRejected = {
  test() {
    // Object elements pass the conversion in both and reach the native
    // serializer, which transfers only ArrayBuffers.
    for (const item of [{}, [], () => {}, new Map()]) {
      throws(() => structuredClone(1, { transfer: [item] }), {
        name: 'TypeError',
        message: CPP_BAD_ITEM,
      });
    }
  },
};

export const arrayIteratorUse = {
  test() {
    const saved = Array.prototype[Symbol.iterator];
    let calls = 0;
    Array.prototype[Symbol.iterator] = function () {
      calls++;
      return saved.call(this);
    };
    const buffer = new ArrayBuffer(4);
    let clone;
    try {
      clone = structuredClone(buffer, { transfer: [buffer] });
    } finally {
      Array.prototype[Symbol.iterator] = saved;
    }
    strictEqual(calls, usingTsImpl ? 1 : 0);
    deepStrictEqual([buffer.byteLength, clone.byteLength], [0, 4]);
  },
};

export const elementGetters = {
  test() {
    // Each element is read once in both.
    const buffer = new ArrayBuffer(4);
    let reads = 0;
    const transfer = [];
    Object.defineProperty(transfer, 0, {
      get() {
        reads++;
        return buffer;
      },
    });
    deepStrictEqual(transferred(transfer, buffer), [0, 4]);
    strictEqual(reads, 1);
  },
};
