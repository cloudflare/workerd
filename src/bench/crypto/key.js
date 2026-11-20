// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Batches of calls that make or read a CryptoKey, where the cost of the key object is largest next
// to the cryptography. Run against the C++ class (key-cpp) and the TypeScript one
// (key-ts).
import * as nodeCrypto from 'node:crypto';

const subtle = crypto.subtle;

// Each case makes this many calls, so that the per-call cost is well above the runner's own.
const BATCH = 64;

function batch(fn) {
  return async () => {
    const promises = new Array(BATCH);
    for (let i = 0; i < BATCH; i++) promises[i] = fn();
    return Promise.all(promises);
  };
}

export default {
  async bench(b) {
    const raw = new Uint8Array(32);
    const hmacParams = { name: 'HMAC', hash: 'SHA-256' };
    const hmac = await subtle.importKey('raw', raw, hmacParams, true, [
      'sign',
      'verify',
    ]);
    const jwk = await subtle.exportKey('jwk', hmac);
    const secret = nodeCrypto.createSecretKey(raw);

    b.run(
      'generateKey HMAC',
      batch(() => subtle.generateKey(hmacParams, false, ['sign']))
    );
    b.run(
      'generateKey Ed25519 pair',
      batch(() => subtle.generateKey('Ed25519', false, ['sign', 'verify']))
    );
    b.run(
      'importKey raw HMAC',
      batch(() => subtle.importKey('raw', raw, hmacParams, false, ['sign']))
    );
    b.run(
      'importKey jwk HMAC',
      batch(() => subtle.importKey('jwk', jwk, hmacParams, false, ['sign']))
    );
    b.run('read algorithm', () => {
      for (let i = 0; i < BATCH; i++) b.blackBox(hmac.algorithm);
    });
    b.run('read usages', () => {
      for (let i = 0; i < BATCH; i++) b.blackBox(hmac.usages);
    });
    b.run('read type', () => {
      for (let i = 0; i < BATCH; i++) b.blackBox(hmac.type);
    });
    b.run('instanceof CryptoKey', () => {
      for (let i = 0; i < BATCH; i++) b.blackBox(hmac instanceof CryptoKey);
    });
    b.run('KeyObject.from', () => {
      for (let i = 0; i < BATCH; i++)
        b.blackBox(nodeCrypto.KeyObject.from(hmac));
    });
    b.run('KeyObject toCryptoKey', () => {
      for (let i = 0; i < BATCH; i++)
        b.blackBox(secret.toCryptoKey(hmacParams, false, ['sign']));
    });
    // The TypeScript CryptoKey makes `algorithm` and `usages` with the key, the C++ one when first
    // read.
    b.run('toCryptoKey, read algorithm and usages', () => {
      for (let i = 0; i < BATCH; i++) {
        const key = secret.toCryptoKey(hmacParams, false, ['sign']);
        b.blackBox(key.algorithm);
        b.blackBox(key.usages);
      }
    });
  },
};
