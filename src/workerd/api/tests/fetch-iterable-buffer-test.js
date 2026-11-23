// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, strictEqual } from 'node:assert';

const enc = new TextEncoder();

// Returns a subclass of `Base` whose iteration is observable: each lookup of an iterator method
// is recorded in `lookups`, and iterating an instance yields "iterated" rather than its contents.
function observeIteration(Base, lookups) {
  return class extends Base {
    get [Symbol.asyncIterator]() {
      lookups.push(`${Base.name}[Symbol.asyncIterator]`);
      return undefined;
    }
    get [Symbol.iterator]() {
      lookups.push(`${Base.name}[Symbol.iterator]`);
      return function* () {
        yield enc.encode('iterated');
      };
    }
  };
}

// Buffer sources of each kind holding the bytes of "Hello".
function helloBufferSources(lookups) {
  const hello = enc.encode('Hello');
  const buffer = new (observeIteration(ArrayBuffer, lookups))(hello.byteLength);
  new Uint8Array(buffer).set(hello);
  return [
    new (observeIteration(Uint8Array, lookups))(hello),
    new (observeIteration(DataView, lookups))(hello.buffer),
    buffer,
  ];
}

// A buffer source body is consumed whole: its iterator methods are never looked up, let alone
// called. Typed arrays are sync iterables, so the iterable body path must reject buffer sources
// before it looks for an iterator.
export const bufferSourceBodiesAreNotIterated = {
  async test() {
    const lookups = [];
    for (const body of helloBufferSources(lookups)) {
      strictEqual(await new Response(body).text(), 'Hello');
      const request = new Request('http://example.com', {
        method: 'POST',
        body,
      });
      strictEqual(await request.text(), 'Hello');
    }
    deepStrictEqual(lookups, []);
  },
};

// Arrays are still iterated, so the iterable body path is active in this configuration.
export const arrayBodiesAreIterated = {
  async test() {
    const response = new Response([enc.encode('Hello '), enc.encode('World!')]);
    strictEqual(await response.text(), 'Hello World!');
  },
};
