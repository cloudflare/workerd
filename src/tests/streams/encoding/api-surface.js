// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object shape of TextEncoderStream and TextDecoderStream. Divergences:
// C++ subclasses TransformStream (readable/writable inherited from its
// prototype); TypeScript is standalone with own readable/writable
// accessors. The codec-facing surface (encoding/fatal/ignoreBOM) matches.

import { strictEqual, ok, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const encoderEncoding = {
  test() {
    strictEqual(new TextEncoderStream().encoding, 'utf-8');
  },
};

export const toStringTagBranding = {
  test() {
    for (const [ctor, name] of [
      [TextEncoderStream, 'TextEncoderStream'],
      [TextDecoderStream, 'TextDecoderStream'],
    ]) {
      strictEqual(
        Object.prototype.toString.call(Reflect.construct(ctor, [])),
        `[object ${name}]`
      );
      const desc = Object.getOwnPropertyDescriptor(
        ctor.prototype,
        Symbol.toStringTag
      );
      strictEqual(desc.value, name);
      strictEqual(desc.enumerable, false);
    }
  },
};

export const transformStreamInheritance = {
  test() {
    // C++ subclasses TransformStream; TypeScript deliberately does not
    // (CompressionStream/IdentityTransformStream convention).
    const tes = new TextEncoderStream();
    const tds = new TextDecoderStream();
    strictEqual(tes instanceof TransformStream, !usingTsImpl);
    strictEqual(tds instanceof TransformStream, !usingTsImpl);
    const parent = Object.getPrototypeOf(TextEncoderStream.prototype);
    strictEqual(
      parent,
      usingTsImpl ? Object.prototype : TransformStream.prototype
    );
  },
};

export const accessorPlacement = {
  test() {
    // encoding/fatal/ignoreBOM: own enumerable get-accessors on the class
    // prototype in both implementations.
    for (const [proto, keys] of [
      [TextEncoderStream.prototype, ['encoding']],
      [TextDecoderStream.prototype, ['encoding', 'fatal', 'ignoreBOM']],
    ]) {
      for (const key of keys) {
        const desc = Object.getOwnPropertyDescriptor(proto, key);
        strictEqual(typeof desc.get, 'function');
        strictEqual(desc.enumerable, true);
      }
    }
    // readable/writable: own on the class prototype under TypeScript,
    // inherited from TransformStream.prototype under C++.
    for (const proto of [
      TextEncoderStream.prototype,
      TextDecoderStream.prototype,
    ]) {
      for (const key of ['readable', 'writable']) {
        const own = Object.getOwnPropertyDescriptor(proto, key);
        if (usingTsImpl) {
          strictEqual(typeof own.get, 'function');
          strictEqual(own.enumerable, true);
        } else {
          strictEqual(own, undefined);
          const inherited = Object.getOwnPropertyDescriptor(
            TransformStream.prototype,
            key
          );
          strictEqual(typeof inherited.get, 'function');
        }
      }
    }
    // Instances carry no own properties.
    strictEqual(Object.getOwnPropertyNames(new TextEncoderStream()).length, 0);
    strictEqual(Object.getOwnPropertyNames(new TextDecoderStream()).length, 0);
  },
};

export const sidesAreStableStreamInstances = {
  test() {
    for (const stream of [new TextEncoderStream(), new TextDecoderStream()]) {
      ok(stream.readable instanceof ReadableStream);
      ok(stream.writable instanceof WritableStream);
      strictEqual(stream.readable, stream.readable);
      strictEqual(stream.writable, stream.writable);
    }
  },
};

export const prototypeAccessorBrandChecks = {
  test() {
    // Getters reject foreign receivers with TypeError; the message prefix
    // is shared (C++ appends explanatory detail).
    for (const [proto, key] of [
      [TextEncoderStream.prototype, 'encoding'],
      [TextEncoderStream.prototype, 'readable'],
      [TextDecoderStream.prototype, 'fatal'],
      [TextDecoderStream.prototype, 'writable'],
    ]) {
      const target = usingTsImpl
        ? proto
        : key === 'readable' || key === 'writable'
          ? TransformStream.prototype
          : proto;
      throws(
        () => Reflect.get(target, key, {}),
        (err) => {
          strictEqual(err.constructor, TypeError);
          ok(err.message.startsWith('Illegal invocation'));
          return true;
        }
      );
    }
  },
};

export const constructorSurface = {
  test() {
    strictEqual(TextEncoderStream.name, 'TextEncoderStream');
    strictEqual(TextDecoderStream.name, 'TextDecoderStream');
    strictEqual(TextEncoderStream.length, 0);
    strictEqual(TextDecoderStream.length, 0);
    // Native source text under C++ only.
    strictEqual(
      String(TextEncoderStream).includes('native code'),
      !usingTsImpl
    );
  },
};
