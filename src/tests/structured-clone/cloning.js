// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The clone itself, which both implementations leave to the native
// serializer: these assertions hold identically.

import {
  deepStrictEqual,
  notStrictEqual,
  ok,
  strictEqual,
  throws,
} from 'node:assert';

export const plainValues = {
  test() {
    for (const value of [undefined, null, true, 0, -0, NaN, 1n, 'str']) {
      ok(Object.is(structuredClone(value), value));
    }
    const value = {
      a: [1, { b: new Map([[1, new Set([2])]]) }],
      d: new Date(0),
    };
    value.self = value;
    const clone = structuredClone(value);
    notStrictEqual(clone, value);
    strictEqual(clone.self, clone);
    deepStrictEqual(clone.a[1].b, new Map([[1, new Set([2])]]));
    strictEqual(clone.d.getTime(), 0);
  },
};

export const hostObjects = {
  test() {
    const headers = new Headers({ a: '1' });
    const clone = structuredClone(headers);
    ok(clone instanceof Headers);
    notStrictEqual(clone, headers);
    strictEqual(clone.get('a'), '1');
  },
};

export const uncloneableValues = {
  test() {
    for (const [value, text] of [
      [() => {}, '() => {} could not be cloned.'],
      [Symbol('s'), 'Symbol(s) could not be cloned.'],
      [new WeakMap(), '#<WeakMap> could not be cloned.'],
    ]) {
      throws(
        () => structuredClone(value),
        (e) => {
          ok(e instanceof DOMException);
          strictEqual(e.name, 'DataCloneError');
          strictEqual(e.code, 25);
          strictEqual(e.message, text);
          return true;
        }
      );
    }
  },
};

export const valueGetterThrows = {
  test() {
    const error = new RangeError('inner');
    throws(
      () =>
        structuredClone({
          get x() {
            throw error;
          },
        }),
      (e) => e === error
    );
  },
};

export const transferSemantics = {
  test() {
    {
      // A view transfers its buffer.
      const view = new Uint8Array([1, 2, 3]);
      const clone = structuredClone(view, { transfer: [view] });
      strictEqual(view.byteLength, 0);
      deepStrictEqual([...clone], [1, 2, 3]);
    }
    {
      // Duplicates and an already-detached buffer are accepted.
      const buffer = new ArrayBuffer(4);
      structuredClone(buffer, { transfer: [buffer, buffer] });
      strictEqual(buffer.byteLength, 0);
      structuredClone(buffer, { transfer: [buffer] });
    }
    {
      // A transferred buffer is detached only after the value is written.
      const buffer = new ArrayBuffer(4);
      const value = {
        get size() {
          return buffer.byteLength;
        },
      };
      strictEqual(structuredClone(value, { transfer: [buffer] }).size, 4);
      strictEqual(buffer.byteLength, 0);
    }
    {
      // Shared memory is not transferable: a SharedArrayBuffer is not an
      // ArrayBuffer, and a view over one has a buffer that cannot be detached.
      const shared = new SharedArrayBuffer(4);
      throws(() => structuredClone(shared, { transfer: [shared] }), {
        name: 'TypeError',
        message: 'Object is not transferable',
      });
      const view = new Uint8Array(shared);
      throws(
        () => structuredClone(view, { transfer: [view] }),
        (e) =>
          e instanceof DOMException &&
          e.name === 'DataCloneError' &&
          e.message === 'Object is not transferable.'
      );
    }
  },
};
