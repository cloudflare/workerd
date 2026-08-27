// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object shape of CompressionStream and DecompressionStream.

import { strictEqual, ok, throws } from 'node:assert';

export const toStringTagBranding = {
  test() {
    strictEqual(
      Object.prototype.toString.call(new CompressionStream('gzip')),
      '[object CompressionStream]'
    );
    strictEqual(
      Object.prototype.toString.call(new DecompressionStream('gzip')),
      '[object DecompressionStream]'
    );
  },
};

export const codecFactoryNotExposed = {
  test() {
    // The internal C++ codec factory is injected through the bootstrap's
    // utils pseudo-global, never a JS-visible surface.
    strictEqual('newCodec' in CompressionStream, false);
    strictEqual('newCodec' in DecompressionStream, false);
    strictEqual('newCompressionCodec' in globalThis, false);
  },
};

export const sidesAreStableStreamInstances = {
  test() {
    for (const stream of [
      new CompressionStream('gzip'),
      new DecompressionStream('gzip'),
    ]) {
      ok(stream.readable instanceof ReadableStream);
      ok(stream.writable instanceof WritableStream);
      strictEqual(stream.readable, stream.readable);
      strictEqual(stream.writable, stream.writable);
    }
  },
};

export const accessorBrandChecks = {
  test() {
    // Prototype getters reject foreign receivers. Which prototype carries
    // readable/writable differs by implementation; resolve dynamically.
    for (const Ctor of [CompressionStream, DecompressionStream]) {
      const instance = new Ctor('gzip');
      for (const key of ['readable', 'writable']) {
        let proto = Object.getPrototypeOf(instance);
        let desc;
        while (proto && !desc) {
          desc = Object.getOwnPropertyDescriptor(proto, key);
          proto = Object.getPrototypeOf(proto);
        }
        strictEqual(typeof desc.get, 'function');
        throws(() => desc.get.call({}), TypeError);
      }
    }
  },
};
