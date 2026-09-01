// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object shape of CompressionStream and DecompressionStream. Divergences:
// C++ subclasses TransformStream (readable/writable inherited from its
// prototype, constructor.length 0, native source text); TypeScript is
// standalone (own accessors, length 1, non-native source).

import { strictEqual, ok, throws } from 'node:assert';
import {
  CompressionStream as NodeCS,
  DecompressionStream as NodeDS,
} from 'node:stream/web';
import { usingTsImpl } from 'which-impl';

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

export const transformStreamInheritance = {
  test() {
    for (const Ctor of [CompressionStream, DecompressionStream]) {
      const instance = new Ctor('gzip');
      strictEqual(instance instanceof TransformStream, !usingTsImpl);
      strictEqual(
        Object.getPrototypeOf(Ctor.prototype),
        usingTsImpl ? Object.prototype : TransformStream.prototype
      );
      // readable/writable: own accessors on the class prototype under
      // TypeScript, inherited from TransformStream.prototype under C++.
      for (const key of ['readable', 'writable']) {
        const own = Object.getOwnPropertyDescriptor(Ctor.prototype, key);
        if (usingTsImpl) {
          strictEqual(typeof own.get, 'function');
        } else {
          strictEqual(own, undefined);
          strictEqual(
            typeof Object.getOwnPropertyDescriptor(
              TransformStream.prototype,
              key
            ).get,
            'function'
          );
        }
      }
      strictEqual(Object.getOwnPropertyNames(instance).length, 0);
    }
  },
};

export const constructorSurface = {
  test() {
    strictEqual(CompressionStream.name, 'CompressionStream');
    strictEqual(DecompressionStream.name, 'DecompressionStream');
    strictEqual(CompressionStream.length, usingTsImpl ? 1 : 0);
    strictEqual(DecompressionStream.length, usingTsImpl ? 1 : 0);
    strictEqual(
      String(CompressionStream).includes('native code'),
      !usingTsImpl
    );
  },
};

export const nodeStreamWebAliases = {
  test() {
    // node:stream/web re-exports the very same classes.
    strictEqual(NodeCS, CompressionStream);
    strictEqual(NodeDS, DecompressionStream);
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
