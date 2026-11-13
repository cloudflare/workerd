// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Batches of SubtleCrypto calls on small inputs, where the cost of the interface (argument conversion, the
// algorithm dictionary, the promise) is largest next to the cryptography. Run against the C++
// class (crypto-subtle-bench) and the TypeScript one (crypto-subtle-ts-bench).

const subtle = crypto.subtle;

// Each case makes this many calls and awaits them together, so that the per-call cost is well
// above the runner's own cost of awaiting a promise.
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
    const data = new Uint8Array(64);
    const iv = new Uint8Array(12);
    const raw = new Uint8Array(16);
    const aes = await subtle.importKey('raw', raw, 'AES-GCM', true, [
      'encrypt',
      'decrypt',
    ]);
    const hmac = await subtle.importKey(
      'raw',
      raw,
      { name: 'HMAC', hash: 'SHA-256' },
      false,
      ['sign', 'verify']
    );
    const hkdf = await subtle.importKey('raw', raw, 'HKDF', false, [
      'deriveBits',
    ]);
    const signature = await subtle.sign('HMAC', hmac, data);
    const jwk = await subtle.exportKey('jwk', aes);
    const hkdfParams = { name: 'HKDF', hash: 'SHA-256', salt: raw, info: raw };

    b.run(
      'digest SHA-256 string',
      batch(() => subtle.digest('SHA-256', data))
    );
    b.run(
      'digest SHA-256 dict',
      batch(() => subtle.digest({ name: 'SHA-256' }, data))
    );
    b.run(
      'encrypt AES-GCM',
      batch(() => subtle.encrypt({ name: 'AES-GCM', iv }, aes, data))
    );
    b.run(
      'sign HMAC',
      batch(() => subtle.sign('HMAC', hmac, data))
    );
    b.run(
      'verify HMAC',
      batch(() => subtle.verify('HMAC', hmac, signature, data))
    );
    b.run(
      'importKey raw AES-GCM',
      batch(() => subtle.importKey('raw', raw, 'AES-GCM', false, ['encrypt']))
    );
    b.run(
      'importKey jwk AES-GCM',
      batch(() => subtle.importKey('jwk', jwk, 'AES-GCM', false, ['encrypt']))
    );
    b.run(
      'exportKey raw',
      batch(() => subtle.exportKey('raw', aes))
    );
    b.run(
      'generateKey AES-GCM',
      batch(() =>
        subtle.generateKey({ name: 'AES-GCM', length: 128 }, false, ['encrypt'])
      )
    );
    b.run(
      'deriveBits HKDF',
      batch(() => subtle.deriveBits(hkdfParams, hkdf, 128))
    );
    b.run('timingSafeEqual', () => {
      for (let i = 0; i < BATCH; i++)
        b.blackBox(subtle.timingSafeEqual(data, data));
    });
  },
};
