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

export const cryptoStubsThrowTest = {
  async test() {
    assert.throws(() => encapsulate({}), /not implemented/i);
    assert.throws(() => decapsulate({}, new Uint8Array()), /not implemented/i);
    assert.throws(() => argon2Sync('pw', 'salt'), /not implemented/i);
    assert.throws(() => crypto.prng(16), /not implemented/i);
    assert.throws(() => crypto.rng(16), /not implemented/i);

    // Callback form of argon2 should deliver the error asynchronously.
    await new Promise((resolve, reject) => {
      argon2('pw', 'salt', {}, (err) => {
        try {
          assert.ok(err);
          assert.match(String(err.message || err), /not implemented/i);
          resolve();
        } catch (e) {
          reject(e);
        }
      });
    });

    // Without callback, argon2 throws synchronously.
    assert.throws(() => argon2('pw', 'salt', {}), /not implemented/i);
  },
};

export const subtleStubsExistTest = {
  async test() {
    const subtle = crypto.webcrypto.subtle;
    assert.strictEqual(subtle, crypto.subtle);
    assert.strictEqual(subtle, globalThis.crypto.subtle);

    assert.strictEqual(typeof subtle.encapsulateKey, 'function');
    assert.strictEqual(typeof subtle.decapsulateKey, 'function');
    assert.strictEqual(typeof subtle.encapsulateBits, 'function');
    assert.strictEqual(typeof subtle.decapsulateBits, 'function');
    assert.strictEqual(typeof subtle.getPublicKey, 'function');

    await assert.rejects(() => subtle.encapsulateKey(), /not implemented/i);
    await assert.rejects(() => subtle.decapsulateKey(), /not implemented/i);
    await assert.rejects(() => subtle.encapsulateBits(), /not implemented/i);
    await assert.rejects(() => subtle.decapsulateBits(), /not implemented/i);
    await assert.rejects(() => subtle.getPublicKey(), /not implemented/i);
  },
};
