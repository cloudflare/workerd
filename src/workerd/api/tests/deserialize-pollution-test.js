// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Values read back while every property of the prototypes their objects inherit from is a counting
// accessor, and so is every name below that those prototypes lack. A counting Proxy, below
// Object.prototype, also sees a lookup of any name that the prototypes other than Object.prototype
// lack. No JavaScript can run while V8 reads a value, so a reader that reached one of them would
// fail the read; work that runs JavaScript waits until the value has been read. The accessors and
// the Proxy keep the original behavior, so every value must come back intact.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

export class Echo extends WorkerEntrypoint {
  echo(value) {
    return value;
  }
}

// Names a reader might look up that may be on none of the prototypes. The Proxy sees them on
// all but plain objects, whose prototype, Object.prototype, can't have one of its own.
const NAMES = [
  'name',
  'message',
  'stack',
  'cause',
  'code',
  'errors',
  'overloaded',
  'retryable',
  'remote',
  'toJSON',
  'toString',
  'valueOf',
  'then',
  'constructor',
  'length',
  'size',
  'type',
  'value',
  'done',
  // The fields of the initializers that Request and Response read.
  'method',
  'headers',
  'body',
  'redirect',
  'cf',
  'signal',
  'integrity',
  'duplex',
  'fetcher',
  'cache',
  'status',
  'statusText',
  'webSocket',
  'encodeBody',
  Symbol.toPrimitive,
  Symbol.iterator,
];

const PROTOTYPES = [
  Object.prototype,
  Array.prototype,
  Error.prototype,
  RangeError.prototype,
  TypeError.prototype,
  DOMException.prototype,
  Map.prototype,
  Set.prototype,
];

let calls = 0;

// Counts every lookup, check and assignment that reaches it, and forwards it to Object.prototype.
// The handler has no prototype, so that looking a trap up doesn't reach a polluted property.
const belowObjectPrototype = new Proxy(Object.prototype, {
  __proto__: null,
  get(target, key, receiver) {
    calls++;
    return Reflect.get(target, key, receiver);
  },
  has(target, key) {
    calls++;
    return Reflect.has(target, key);
  },
  set(target, key, value, receiver) {
    calls++;
    return Reflect.set(target, key, value, receiver);
  },
});

// Replaces each property of each prototype, and each of NAMES, with an accessor that counts its
// calls and otherwise behaves as the property did, and puts belowObjectPrototype between
// Object.prototype and the prototypes that inherit from it. Returns a function that undoes it.
// The descriptors have no prototype, since Object.prototype's `value` is among the properties
// replaced.
function pollute() {
  const restore = [];
  for (const prototype of PROTOTYPES) {
    if (Object.getPrototypeOf(prototype) === Object.prototype) {
      Object.setPrototypeOf(prototype, belowObjectPrototype);
      restore.push(() => Object.setPrototypeOf(prototype, Object.prototype));
    }
  }
  for (const prototype of PROTOTYPES) {
    for (const name of new Set([...Reflect.ownKeys(prototype), ...NAMES])) {
      const original = Object.getOwnPropertyDescriptor(prototype, name);
      if (original !== undefined && !original.configurable) continue;
      Object.defineProperty(prototype, name, {
        __proto__: null,
        get() {
          calls++;
          if (original === undefined) {
            const parent = Object.getPrototypeOf(prototype);
            return parent === null
              ? undefined
              : Reflect.get(parent, name, this);
          }
          return original.get ? original.get.call(this) : original.value;
        },
        set(value) {
          calls++;
          if (original?.set) {
            original.set.call(this, value);
          } else {
            Object.defineProperty(this, name, {
              __proto__: null,
              value,
              writable: true,
              enumerable: true,
              configurable: true,
            });
          }
        },
        configurable: true,
      });
      restore.push(() => {
        if (original === undefined) {
          Reflect.deleteProperty(prototype, name);
        } else {
          Object.defineProperty(prototype, name, {
            __proto__: null,
            ...original,
          });
        }
      });
    }
  }
  return () => {
    for (const r of restore.reverse()) r();
  };
}

async function withPollution(fn) {
  const restore = pollute();
  try {
    return await fn();
  } finally {
    restore();
  }
}

// V8 fills the Map and the Set, nested in a host object, by calling builtins. The error in the
// Map is a host object nested in them.
function makeError() {
  const error = new RangeError('range', { cause: new TypeError('cause') });
  error.code = 7;
  error.map = new Map([
    ['k', { a: 1 }],
    ['error', new TypeError('in map')],
  ]);
  error.set = new Set([{ b: [2] }]);
  return error;
}

function checkErrorCollections(error) {
  deepStrictEqual(error.map.get('k'), { a: 1 });
  ok(error.map.get('error') instanceof TypeError);
  strictEqual(error.map.get('error').message, 'in map');
  deepStrictEqual([...error.set], [{ b: [2] }]);
}

export const structuredCloneReadsNoPrototypeProperty = {
  async test() {
    calls = 0;
    const copy = await withPollution(() =>
      structuredClone({
        plain: { a: 1, nested: [1, 'two', { three: 3 }] },
        map: new Map([['k', { v: 1 }]]),
        set: new Set([1, 2]),
        date: new Date(0),
        bytes: new Uint8Array([1, 2, 3]),
        error: makeError(),
        domException: new DOMException('dom', 'AbortError'),
        blob: new Blob(['blob'], { type: 'text/plain' }),
        headers: new Headers({ 'x-a': '1' }),
      })
    );
    // The getters are reachable: the writer, which may run JavaScript, calls some.
    ok(calls > 0);
    deepStrictEqual(copy.plain, { a: 1, nested: [1, 'two', { three: 3 }] });
    deepStrictEqual([...copy.map], [['k', { v: 1 }]]);
    deepStrictEqual([...copy.set], [1, 2]);
    strictEqual(copy.date.getTime(), 0);
    deepStrictEqual([...copy.bytes], [1, 2, 3]);
    ok(copy.error instanceof RangeError);
    strictEqual(copy.error.message, 'range');
    strictEqual(copy.error.code, 7);
    ok(copy.error.cause instanceof TypeError);
    strictEqual(copy.error.cause.message, 'cause');
    checkErrorCollections(copy.error);
    ok(copy.domException instanceof DOMException);
    strictEqual(copy.domException.name, 'AbortError');
    strictEqual(copy.domException.message, 'dom');
    strictEqual(await copy.blob.text(), 'blob');
    strictEqual(copy.blob.type, 'text/plain');
    strictEqual(copy.headers.get('x-a'), '1');
  },
};

export const rpcReadsNoPrototypeProperty = {
  async test(ctrl, env) {
    calls = 0;
    // Each is read twice: by the callee, and by the caller from what the callee returns.
    const value = await withPollution(() =>
      env.Echo.echo({
        plain: { a: 1 },
        error: makeError(),
        request: new Request('https://example.com/', {
          method: 'POST',
          headers: { 'x-a': '1' },
          body: 'request body',
          cf: { colo: 'test' },
        }),
        response: new Response('response body', {
          status: 201,
          headers: { 'x-b': '2' },
          cf: { colo: 'test' },
        }),
        aborted: [
          AbortSignal.abort({ plain: true }),
          AbortSignal.abort(makeError()),
          AbortSignal.abort(new DOMException('dom', 'AbortError')),
          AbortSignal.abort(),
        ],
        pending: new AbortController().signal,
        blob: new Blob(['blob']),
        headers: new Headers({ 'x-c': '3' }),
        stream: new Blob(['stream']).stream(),
      })
    );
    ok(calls > 0);
    deepStrictEqual(value.plain, { a: 1 });
    ok(value.error instanceof RangeError);
    strictEqual(value.error.cause.message, 'cause');
    checkErrorCollections(value.error);

    strictEqual(value.request.method, 'POST');
    strictEqual(value.request.headers.get('x-a'), '1');
    deepStrictEqual(value.request.cf, { colo: 'test' });
    strictEqual(await value.request.text(), 'request body');
    strictEqual(value.response.status, 201);
    strictEqual(value.response.headers.get('x-b'), '2');
    deepStrictEqual(value.response.cf, { colo: 'test' });
    strictEqual(await value.response.text(), 'response body');

    const [plain, error, domException, none] = value.aborted;
    for (const signal of value.aborted) ok(signal.aborted);
    deepStrictEqual(plain.reason, { plain: true });
    ok(error.reason instanceof RangeError);
    strictEqual(error.reason.message, 'range');
    checkErrorCollections(error.reason);
    ok(domException.reason instanceof DOMException);
    strictEqual(domException.reason.message, 'dom');
    ok(none.reason instanceof DOMException);
    strictEqual(none.reason.name, 'AbortError');
    ok(!value.pending.aborted);

    strictEqual(await value.blob.text(), 'blob');
    strictEqual(value.headers.get('x-c'), '3');
    strictEqual(await new Response(value.stream).text(), 'stream');
  },
};
