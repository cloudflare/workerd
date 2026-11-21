// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects } from 'node:assert';

// None of SubtleCrypto's BufferSource arguments or dictionary members are
// [AllowShared], so a view onto a SharedArrayBuffer is a TypeError. The WPT
// sharedarraybuffer.https.any.js covers digest() and AES-GCM encrypt(); this
// covers the rest.
const sharedBuffer = new WebAssembly.Memory({
  shared: true,
  initial: 1,
  maximum: 1,
}).buffer;

// A Uint8Array onto the shared memory holding a copy of `bytes`, which is a
// byte length (for zeros), an array of bytes or an ArrayBuffer.
function shared(bytes) {
  const source = new Uint8Array(bytes);
  const view = new Uint8Array(sharedBuffer, 0, source.byteLength);
  view.set(source);
  return view;
}

const subtle = crypto.subtle;

// The check throws where the other argument TypeErrors do, which is synchronous
// at old compatibility dates, so the call goes inside an async function.
async function rejectsShared(name, makeCall) {
  await rejects(async () => makeCall(), TypeError, name);
}

export const dataArguments = {
  async test() {
    const aes = await subtle.generateKey(
      { name: 'AES-GCM', length: 128 },
      true,
      ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey']
    );
    const iv = new Uint8Array(12);
    const ciphertext = await subtle.encrypt(
      { name: 'AES-GCM', iv },
      aes,
      new Uint8Array(16)
    );
    await rejectsShared('decrypt data', () =>
      subtle.decrypt({ name: 'AES-GCM', iv }, aes, shared(ciphertext))
    );

    const hmac = await subtle.generateKey(
      { name: 'HMAC', hash: 'SHA-256' },
      false,
      ['sign', 'verify']
    );
    const signature = await subtle.sign('HMAC', hmac, new Uint8Array(4));
    await rejectsShared('sign data', () =>
      subtle.sign('HMAC', hmac, shared(4))
    );
    await rejectsShared('verify signature', () =>
      subtle.verify('HMAC', hmac, shared(signature), new Uint8Array(4))
    );
    await rejectsShared('verify data', () =>
      subtle.verify('HMAC', hmac, signature, shared(4))
    );

    await rejectsShared('importKey keyData', () =>
      subtle.importKey('raw', shared(16), 'AES-GCM', false, ['encrypt'])
    );

    const toWrap = await subtle.generateKey(
      { name: 'AES-GCM', length: 128 },
      true,
      ['encrypt']
    );
    const wrapped = await subtle.wrapKey('raw', toWrap, aes, {
      name: 'AES-GCM',
      iv,
    });
    await rejectsShared('unwrapKey wrappedKey', () =>
      subtle.unwrapKey(
        'raw',
        shared(wrapped),
        aes,
        { name: 'AES-GCM', iv },
        'AES-GCM',
        false,
        ['encrypt']
      )
    );
    // The same bytes, unshared, unwrap fine.
    await subtle.unwrapKey(
      'raw',
      new Uint8Array(wrapped),
      aes,
      { name: 'AES-GCM', iv },
      'AES-GCM',
      false,
      ['encrypt']
    );
  },
};

export const algorithmMembers = {
  async test() {
    const ctr = await subtle.generateKey(
      { name: 'AES-CTR', length: 128 },
      false,
      ['encrypt']
    );
    await rejectsShared('AES-CTR counter', () =>
      subtle.encrypt(
        { name: 'AES-CTR', counter: shared(16), length: 64 },
        ctr,
        new Uint8Array(1)
      )
    );

    const aes = await subtle.generateKey(
      { name: 'AES-GCM', length: 128 },
      true,
      ['encrypt', 'wrapKey']
    );
    await rejectsShared('wrapKey iv', () =>
      subtle.wrapKey('raw', aes, aes, {
        name: 'AES-GCM',
        iv: shared(12),
      })
    );

    const rsa = await subtle.generateKey(
      {
        name: 'RSA-OAEP',
        modulusLength: 1024,
        publicExponent: new Uint8Array([1, 0, 1]),
        hash: 'SHA-256',
      },
      false,
      ['encrypt', 'decrypt']
    );
    await rejectsShared('RSA-OAEP label', () =>
      subtle.encrypt(
        { name: 'RSA-OAEP', label: shared(4) },
        rsa.publicKey,
        new Uint8Array(1)
      )
    );
    await rejectsShared('RSA publicExponent', () =>
      subtle.generateKey(
        {
          name: 'RSA-OAEP',
          modulusLength: 1024,
          publicExponent: shared([1, 0, 1]),
          hash: 'SHA-256',
        },
        false,
        ['encrypt']
      )
    );

    const hkdf = await subtle.importKey(
      'raw',
      new Uint8Array(16),
      'HKDF',
      false,
      ['deriveBits', 'deriveKey']
    );
    await rejectsShared('HKDF salt', () =>
      subtle.deriveBits(
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: shared(4),
          info: new Uint8Array(),
        },
        hkdf,
        128
      )
    );
    await rejectsShared('HKDF info', () =>
      subtle.deriveKey(
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: new Uint8Array(),
          info: shared(4),
        },
        hkdf,
        { name: 'AES-GCM', length: 128 },
        false,
        ['encrypt']
      )
    );

    const mldsa = await subtle.generateKey('ML-DSA-44', false, ['sign']);
    await rejectsShared('ML-DSA context', () =>
      subtle.sign(
        { name: 'ML-DSA-44', context: shared(4) },
        mldsa.privateKey,
        new Uint8Array(1)
      )
    );
  },
};

export const decapsulateCiphertext = {
  async test() {
    const keyPair = await subtle.generateKey('ML-KEM-768', false, [
      'encapsulateBits',
      'decapsulateBits',
      'decapsulateKey',
    ]);
    const { ciphertext } = await subtle.encapsulateBits(
      'ML-KEM-768',
      keyPair.publicKey
    );
    await rejectsShared('decapsulateBits ciphertext', () =>
      subtle.decapsulateBits(
        'ML-KEM-768',
        keyPair.privateKey,
        shared(ciphertext)
      )
    );
    await rejectsShared('decapsulateKey ciphertext', () =>
      subtle.decapsulateKey(
        'ML-KEM-768',
        keyPair.privateKey,
        shared(ciphertext),
        'AES-GCM',
        false,
        ['encrypt']
      )
    );
    // The same bytes, unshared, decapsulate fine.
    await subtle.decapsulateBits(
      'ML-KEM-768',
      keyPair.privateKey,
      new Uint8Array(ciphertext)
    );
  },
};
