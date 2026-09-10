// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object shape of crypto.DigestStream: a subclass of the global
// WritableStream in both implementations, with brand-checked accessors.
// Divergence: TypeScript uses a genuine `class extends`, so the
// CONSTRUCTOR's prototype is the WritableStream function itself; the C++
// jsg inheritance wires the instance prototype chain but not the static
// constructor chain.

import { strictEqual, notStrictEqual, ok, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const isRealWritableStreamSubclass = {
  test() {
    const stream = new crypto.DigestStream('md5');
    ok(stream instanceof WritableStream);
    strictEqual(
      Object.getPrototypeOf(crypto.DigestStream.prototype),
      WritableStream.prototype
    );
    if (usingTsImpl) {
      strictEqual(Object.getPrototypeOf(crypto.DigestStream), WritableStream);
    } else {
      notStrictEqual(
        Object.getPrototypeOf(crypto.DigestStream),
        WritableStream
      );
    }
    // Inherited members must be reachable, not shadowed.
    strictEqual(typeof stream.getWriter, 'function');
    strictEqual(stream.locked, false);
  },
};

export const toStringTagBranding = {
  test() {
    strictEqual(
      Object.prototype.toString.call(new crypto.DigestStream('md5')),
      '[object DigestStream]'
    );
  },
};

export const accessorsAreBrandChecked = {
  test() {
    // Brand checks, not instanceof checks: a plain object with the right
    // prototype is still rejected.
    const fake = Object.create(crypto.DigestStream.prototype);
    throws(() => fake.digest, { name: 'TypeError' });
    throws(() => fake.bytesWritten, { name: 'TypeError' });
    throws(() => fake[Symbol.dispose](), { name: 'TypeError' });

    const desc = Object.getOwnPropertyDescriptor(
      crypto.DigestStream.prototype,
      'digest'
    );
    strictEqual(typeof desc.get, 'function');
    strictEqual(desc.set, undefined);
  },
};

export const constructorIsSubclassable = {
  async test() {
    class MyDigest extends crypto.DigestStream {
      constructor() {
        super('md5');
        this.tag = 'mine';
      }
    }
    const stream = new MyDigest();
    strictEqual(stream.tag, 'mine');
    ok(stream instanceof crypto.DigestStream);
    ok(stream instanceof WritableStream);
    const writer = stream.getWriter();
    await writer.write('hello');
    await writer.close();
    strictEqual((await stream.digest).byteLength, 16);
  },
};
