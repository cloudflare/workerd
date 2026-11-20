// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Where the TypeScript CryptoKey knowingly differs from the C++ one. Each case is listed in
// src/per_isolate/crypto/AGENTS.md. Past the first two, each needs a key whose prototype user code
// has replaced with Object.prototype or null.
import { strictEqual, throws } from 'node:assert';
import * as nodeCrypto from 'node:crypto';

async function reprototypedKey(prototype) {
  const key = await crypto.subtle.importKey(
    'raw',
    new Uint8Array(32),
    { name: 'HMAC', hash: 'SHA-256' },
    false,
    ['sign']
  );
  Object.setPrototypeOf(key, prototype);
  return key;
}

// The C++ class is a native function; the TypeScript one is not, which is itself a divergence.
const isTs = !Function.prototype.toString
  .call(CryptoKey)
  .includes('[native code]');

export const sourceText = {
  test() {
    strictEqual(
      Function.prototype.toString.call(CryptoKey).includes('[native code]'),
      !isTs
    );
  },
};

// The C++ class's error begins at the caller; the TypeScript one's in the bootstrap.
export const constructorStack = {
  test() {
    let stack;
    try {
      new CryptoKey();
    } catch (e) {
      stack = e.stack;
    }
    strictEqual(stack.split('\n')[1].includes('per-isolate'), isTs);
  },
};

const unserializable = {
  name: 'DataCloneError',
  message:
    'Could not serialize object of type "CryptoKey". This type does not support serialization.',
};

export const structuredCloneWithObjectPrototype = {
  async test() {
    const key = await reprototypedKey(Object.prototype);
    if (isTs) {
      // The serializer honours the uncloneable mark only on objects with a class prototype, so
      // the clone is a plain copy of the four public properties. No key material is copied.
      const copy = structuredClone(key);
      strictEqual(
        JSON.stringify(Object.keys(copy)),
        '["usages","algorithm","extractable","type"]'
      );
      strictEqual(copy.type, 'secret');
    } else {
      throws(() => structuredClone(key), unserializable);
    }
  },
};

export const structuredCloneWithNullPrototype = {
  async test() {
    const key = await reprototypedKey(null);
    if (isTs) {
      strictEqual(
        Object.getPrototypeOf(structuredClone(key)),
        Object.prototype
      );
    } else {
      throws(() => structuredClone(key), unserializable);
    }
  },
};

export const inspectWithNullPrototype = {
  async test() {
    // Node's argument errors name a value with no prototype by its V8 class name.
    const key = await reprototypedKey(null);
    throws(() => nodeCrypto.KeyObject.from(key), {
      name: 'TypeError',
      message: new RegExp(
        `Received \\[${isTs ? 'Object' : 'CryptoKey'}: null prototype\\]$`
      ),
    });
  },
};
