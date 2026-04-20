// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import crypto, {
  encapsulate,
  decapsulate,
  argon2,
  argon2Sync,
  diffieHellman,
} from 'node:crypto';

export const cryptoNewExportsExistTest = {
  test() {
    // Named exports
    assert.strictEqual(typeof encapsulate, 'function');
    assert.strictEqual(typeof decapsulate, 'function');
    assert.strictEqual(typeof argon2, 'function');
    assert.strictEqual(typeof argon2Sync, 'function');
    assert.strictEqual(typeof diffieHellman, 'function');

    // Default export members
    assert.strictEqual(typeof crypto.encapsulate, 'function');
    assert.strictEqual(typeof crypto.decapsulate, 'function');
    assert.strictEqual(typeof crypto.argon2, 'function');
    assert.strictEqual(typeof crypto.argon2Sync, 'function');
    assert.strictEqual(typeof crypto.diffieHellman, 'function');
    assert.strictEqual(typeof crypto.prng, 'function');
    assert.strictEqual(typeof crypto.rng, 'function');
  },
};

export const cryptoRandomBytesAliasesTest = {
  test() {
    assert.strictEqual(crypto.prng, crypto.randomBytes);
    assert.strictEqual(crypto.rng, crypto.randomBytes);
    assert.strictEqual(crypto.prng(16).length, 16);
  },
};

function expectCallbackError(fn) {
  const { promise, resolve } = Promise.withResolvers();
  let sync = true;
  fn((err) => {
    assert.strictEqual(sync, false);
    assert.strictEqual(err.code, 'ERR_METHOD_NOT_IMPLEMENTED');
    resolve();
  });
  sync = false;
  return promise;
}

export const cryptoStubsThrowTest = {
  async test() {
    const notImplemented = { code: 'ERR_METHOD_NOT_IMPLEMENTED' };
    assert.throws(() => encapsulate({}), notImplemented);
    assert.throws(() => decapsulate({}, new Uint8Array()), notImplemented);
    assert.throws(() => argon2('argon2id', {}), notImplemented);
    assert.throws(() => argon2Sync('argon2id', {}), notImplemented);

    await expectCallbackError((cb) => encapsulate({}, cb));
    await expectCallbackError((cb) => decapsulate({}, new Uint8Array(), cb));
    await expectCallbackError((cb) => argon2('argon2id', {}, cb));
  },
};
