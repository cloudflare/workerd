// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// URLSearchParams iteration: the iterator prototypes, the results, liveness
// under mutation, and forEach().

import { throws, strictEqual, deepStrictEqual, ok } from 'node:assert';
import { usingTsImpl, fastJsgStruct } from 'which-impl';
import { attributes } from 'helpers';

const kIteratorPrototype = Object.getPrototypeOf(
  Object.getPrototypeOf([][Symbol.iterator]())
);

export const iteratorPrototypes = {
  test() {
    const params = new URLSearchParams('a=1');
    const protos = [params.entries(), params.keys(), params.values()].map(
      Object.getPrototypeOf
    );
    for (const proto of protos) {
      strictEqual(Object.getPrototypeOf(proto), kIteratorPrototype);
      const next = Object.getOwnPropertyDescriptor(proto, 'next');
      strictEqual(attributes(next), 'datawec');
      strictEqual(next.value.name, 'next');
      strictEqual(next.value.length, 0);
    }
    // Ledger #6.
    if (usingTsImpl) {
      strictEqual(protos[0], protos[1]);
      strictEqual(protos[0], protos[2]);
      ok(!Object.hasOwn(protos[0], 'constructor'));
    } else {
      strictEqual(new Set(protos).size, 3);
      deepStrictEqual(
        protos.map((p) => p.constructor.name),
        ['EntryIterator', 'KeyIterator', 'ValueIterator']
      );
    }
  },
};

export const defaultIterator = {
  test() {
    const params = new URLSearchParams('a=1&b=2');
    deepStrictEqual(
      [...params],
      [
        ['a', '1'],
        ['b', '2'],
      ]
    );
    deepStrictEqual(
      [...params.entries()],
      [
        ['a', '1'],
        ['b', '2'],
      ]
    );
    deepStrictEqual([...params.keys()], ['a', 'b']);
    deepStrictEqual([...params.values()], ['1', '2']);
    // Each iterator is its own iterable.
    const it = params.keys();
    strictEqual(it[Symbol.iterator](), it);
    strictEqual(
      URLSearchParams.prototype[Symbol.iterator] ===
        URLSearchParams.prototype.entries,
      usingTsImpl
    );
  },
};

export const iteratorResults = {
  test() {
    const it = new URLSearchParams('a=1').entries();
    const first = it.next();
    strictEqual(Object.getPrototypeOf(first), Object.prototype);
    // Ledger #24: CreateIterResultObject defines `value` first.
    deepStrictEqual(
      Reflect.ownKeys(first),
      usingTsImpl ? ['value', 'done'] : ['done', 'value']
    );
    for (const key of ['done', 'value']) {
      strictEqual(
        attributes(Object.getOwnPropertyDescriptor(first, key)),
        'datawec'
      );
    }
    deepStrictEqual(first, { done: false, value: ['a', '1'] });
    ok(Array.isArray(first.value));
    // Ledger #24: without enable_fast_jsg_struct, JSG omits the final
    // result's `value`.
    const last = it.next();
    deepStrictEqual(
      Reflect.ownKeys(last),
      usingTsImpl
        ? ['value', 'done']
        : fastJsgStruct
          ? ['done', 'value']
          : ['done']
    );
    strictEqual(last.done, true);
    strictEqual(last.value, undefined);
  },
};

export const iterationIsLive = {
  test() {
    const params = new URLSearchParams('a=1&b=2&c=3');
    const seen = [];
    for (const [key] of params) {
      seen.push(key);
      if (key === 'a') params.delete('a');
    }
    deepStrictEqual(seen, ['a', 'c']);

    const appended = new URLSearchParams('a=1');
    const keys = appended.keys();
    strictEqual(keys.next().value, 'a');
    appended.append('b', '2');
    strictEqual(keys.next().value, 'b');

    // An exhausted iterator sees later entries: `done` is not sticky.
    const later = new URLSearchParams('a=1');
    const values = later.values();
    values.next();
    strictEqual(values.next().done, true);
    later.append('b', '2');
    deepStrictEqual(values.next(), { done: false, value: '2' });
  },
};

export const iteratorOverUrlSearchParams = {
  test() {
    const url = new URL('http://a/?a=1');
    const it = url.searchParams.keys();
    url.search = 'x=1&y=2';
    deepStrictEqual([...it], ['x', 'y']);
  },
};

export const forEachCallback = {
  test() {
    for (const params of [new URLSearchParams('a=1'), new URLSearchParams()]) {
      for (const callback of [undefined, null, 1, {}]) {
        throws(
          () => params.forEach(callback),
          usingTsImpl
            ? { name: 'TypeError' }
            : {
                name: 'TypeError',
                message:
                  "Failed to execute 'forEach' on 'URLSearchParams': parameter 1 is not of type 'function'.",
              }
        );
      }
    }
    const params = new URLSearchParams('a=1&b=2');
    const calls = [];
    params.forEach(function (...args) {
      calls.push([args.length, args[0], args[1], args[2] === params]);
    });
    deepStrictEqual(calls, [
      [3, '1', 'a', true],
      [3, '2', 'b', true],
    ]);
  },
};

export const forEachReceiver = {
  test() {
    const params = new URLSearchParams('a=1');
    const receivers = [];
    const record = function () {
      receivers.push(this);
    };
    const object = {};
    params.forEach(record);
    params.forEach(record, undefined);
    params.forEach(record, null);
    params.forEach(record, 1);
    params.forEach(record, object);
    // Ledger #18.
    deepStrictEqual(receivers, [
      undefined,
      undefined,
      usingTsImpl ? null : undefined,
      1,
      object,
    ]);
  },
};

export const forEachIsLive = {
  test() {
    const params = new URLSearchParams('a=1&b=2&c=3');
    const seen = [];
    params.forEach((value, key) => {
      seen.push(key);
      if (key === 'a') params.delete('a');
      if (key === 'c') params.append('d', '4');
    });
    deepStrictEqual(seen, ['a', 'c', 'd']);
  },
};
