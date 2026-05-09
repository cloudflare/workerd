// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  createCipheriv,
  createDecipheriv,
  randomBytes,
  createSecretKey,
  publicDecrypt,
  publicEncrypt,
  privateDecrypt,
  privateEncrypt,
  createPublicKey,
  createPrivateKey,
  getCiphers,
  getCipherInfo,
  createCipher,
  createDecipher,
  Cipher,
  Decipher,
} from 'node:crypto';

import { strictEqual, deepStrictEqual, throws, ok } from 'node:assert';

const tests = [
  { name: 'aes-128-cbc', size: 16, iv: 16 },
  { name: 'aes-192-cbc', size: 24, iv: 16 },
  { name: 'aes-256-cbc', size: 32, iv: 16 },
  { name: 'aes-128-ctr', size: 16, iv: 16 },
  { name: 'aes-192-ctr', size: 24, iv: 16 },
  { name: 'aes-256-ctr', size: 32, iv: 16 },
  { name: 'aes-128-ecb', size: 16, iv: 0 },
  { name: 'aes-192-ecb', size: 24, iv: 0 },
  { name: 'aes-256-ecb', size: 32, iv: 0 },
  { name: 'aes-128-ofb', size: 16, iv: 16 },
  { name: 'aes-192-ofb', size: 24, iv: 16 },
  { name: 'aes-256-ofb', size: 32, iv: 16 },
];

const authTagTests = [
  { name: 'aes-128-gcm', size: 16, iv: 16 },
  { name: 'aes-192-gcm', size: 24, iv: 16 },
  { name: 'aes-256-gcm', size: 32, iv: 16 },
  { name: 'chacha20-poly1305', size: 32, iv: 12 },
];

export const cipheriv = {
  async test() {
    tests.forEach((test) => {
      const key = createSecretKey(Buffer.alloc(test.size));
      const iv = Buffer.alloc(test.iv);

      const cipher = createCipheriv(test.name, key, iv);
      const decipher = createDecipheriv(test.name, key, iv);

      let data = '';
      data += decipher.update(cipher.update('Hello World', 'utf8'));
      data += decipher.update(cipher.final());
      data += decipher.final();
      strictEqual(data, 'Hello World');
    });

    // Test that the streams API works also
    await Promise.all(
      tests.map(async (test) => {
        const { promise, resolve, reject } = Promise.withResolvers();

        const key = createSecretKey(Buffer.alloc(test.size));
        const iv = Buffer.alloc(test.iv);

        const cipher = createCipheriv(test.name, key, iv);
        const decipher = createDecipheriv(test.name, key, iv);
        decipher.setEncoding('utf8');

        cipher.end('Hello World');
        cipher.on('data', (chunk) => {
          decipher.write(chunk);
        });
        cipher.on('end', () => {
          decipher.end();
        });

        cipher.on('error', reject);
        decipher.on('error', reject);

        let res = '';
        decipher.on('data', (chunk) => {
          res += chunk;
        });
        decipher.on('end', resolve);

        await promise;
        strictEqual(res, 'Hello World', test.name);
      })
    );

    authTagTests.forEach((test) => {
      const key = createSecretKey(Buffer.alloc(test.size));
      const iv = Buffer.alloc(test.iv);

      const cipher = createCipheriv(test.name, key, iv);

      let data = '';
      cipher.setAAD(Buffer.from('hello'));
      data += cipher.update('Hello World', 'utf8', 'hex');
      data += cipher.final('hex');

      const tag = cipher.getAuthTag();

      //tag[1] = 0xbb;

      const decipher = createDecipheriv(test.name, key, iv);
      decipher.setAuthTag(tag);
      decipher.setAAD(Buffer.from('hello'));
      let res = '';
      res += decipher.update(data, 'hex');
      res += decipher.final();
      strictEqual(res, 'Hello World');
    });
  },
};

export const largeData = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();

    const key = createSecretKey(Buffer.alloc(16));
    const iv = Buffer.alloc(16);
    const cipher = createCipheriv('aes-128-cbc', key, iv);
    const chunks = [
      randomBytes(1024),
      randomBytes(2048),
      randomBytes(4096),
      randomBytes(8192),
      randomBytes(16304),
    ];
    chunks.forEach((chunk) => cipher.write(chunk));
    cipher.end();

    const decipher = createDecipheriv('aes-128-cbc', key, iv);
    cipher.pipe(decipher);

    const output = [];
    decipher.on('data', (chunk) => output.push(chunk));

    decipher.on('end', () => {
      const inputCombined = Buffer.concat(chunks);
      const outputCombined = Buffer.concat(output);
      deepStrictEqual(inputCombined, outputCombined);
      resolve();
    });

    decipher.on('error', reject);

    await promise;
  },
};

export const publicEncryptPrivateDecrypt = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    pub.oaepLabel = 'test';
    pub.oaepHash = 'sha256';
    pub.padding = 4;
    pub.encoding = 'utf8';

    pvt.oaepLabel = 'test';
    pvt.oaepHash = 'sha256';
    pvt.padding = 4;
    pvt.encoding = 'utf8';

    strictEqual(
      privateDecrypt(pvt, publicEncrypt(pub, 'hello')).toString(),
      'hello'
    );
  },
};

export const publicEncryptPrivateDecryptDer = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    const pubDer = {
      key: pub.export({ type: 'pkcs1', format: 'der' }),
      format: 'der',
      type: 'pkcs1',
    };

    const pvtDer = {
      key: pvt.export({ type: 'pkcs8', format: 'der' }),
      format: 'der',
      type: 'pkcs8',
    };

    strictEqual(
      privateDecrypt(pvtDer, publicEncrypt(pubDer, 'hello')).toString(),
      'hello'
    );
  },
};

export const publicEncryptPrivateDecryptPem = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    const pubPem = {
      key: pub.export({ type: 'pkcs1', format: 'pem' }),
      format: 'pem',
      type: 'pkcs1',
    };

    const pvtPem = {
      key: pvt.export({ type: 'pkcs8', format: 'pem' }),
      format: 'pem',
      type: 'pkcs8',
    };

    strictEqual(
      privateDecrypt(pvtPem, publicEncrypt(pubPem, 'hello')).toString(),
      'hello'
    );
  },
};

export const publicEncryptPrivateDecryptPem2 = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    const pubPem = pub.export({ type: 'pkcs1', format: 'pem' });
    const pvtPem = pvt.export({ type: 'pkcs8', format: 'pem' });

    strictEqual(
      privateDecrypt(pvtPem, publicEncrypt(pubPem, 'hello')).toString(),
      'hello'
    );
  },
};

export const publicEncryptPrivateDecryptPem3 = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    const pubPem = pub.export({ type: 'spki', format: 'pem' });
    const pvtPem = pvt.export({ type: 'pkcs8', format: 'pem' });

    strictEqual(
      privateDecrypt(pvtPem, publicEncrypt(pubPem, 'hello')).toString(),
      'hello'
    );
  },
};

export const privateEncryptPublicDecrypt = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    pub.oaepLabel = 'test';
    pub.oaepHash = 'sha256';
    pub.padding = 3;
    pub.encoding = 'utf8';

    pvt.oaepLabel = 'test';
    pvt.oaepHash = 'sha256';
    pvt.padding = 3;
    pvt.encoding = 'utf8';

    const input = 'a'.repeat(256);
    strictEqual(
      publicDecrypt(pub, privateEncrypt(pvt, input)).toString(),
      input
    );
  },
};

export const missingArgChecks = {
  test() {
    throws(() => publicEncrypt(), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => publicDecrypt(), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => privateEncrypt(), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => privateDecrypt(), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => publicEncrypt('key'), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => publicDecrypt('key'), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => privateEncrypt('key'), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => privateDecrypt('key'), {
      code: 'ERR_MISSING_ARGS',
    });
  },
};

export const known_ciphers_info = {
  test() {
    const ciphers = getCiphers();
    ciphers.forEach((i) => {
      const info = getCipherInfo(i);
      ok(info);
      strictEqual(typeof info, 'object');
      strictEqual(typeof info.name, 'string');
      strictEqual(typeof info.nid, 'number');
      strictEqual(typeof info.blockSize, 'number');
      strictEqual(typeof info.ivLength, 'number');
      strictEqual(typeof info.keyLength, 'number');
      strictEqual(typeof info.mode, 'string');
    });
  },
};

export const test_cipher_info = {
  test() {
    const info = getCipherInfo('aes-128-cbc', {
      ivLength: 16,
      keyLength: 16,
    });
    ok(info);
    strictEqual(typeof info, 'object');
    strictEqual(typeof info.name, 'string');
    strictEqual(typeof info.nid, 'number');
    strictEqual(typeof info.blockSize, 'number');
    strictEqual(typeof info.ivLength, 'number');
    strictEqual(typeof info.keyLength, 'number');
    strictEqual(typeof info.mode, 'string');
  },
};

export const test_cipher_info2 = {
  test() {
    const info = getCipherInfo('aes-128-cbc', {
      ivLength: 17,
      keyLength: 15,
    });
    strictEqual(info, undefined);
  },
};

export const test_cipher_info3 = {
  test() {
    const info = getCipherInfo('aes-128-cbc', {
      ivLength: -1,
      keyLength: 16,
    });
    strictEqual(info, undefined);
  },
};

export const test_cipher_info4 = {
  test() {
    const info = getCipherInfo('aes-128-cbc', {
      ivLength: 16,
      keyLength: -1,
    });
    strictEqual(info, undefined);
  },
};

// Verify that modifying a buffer after passing it to a cipher API does not
// affect the cipher output. Buffer data (IV, AAD, auth tag) is copied into
// C++-owned memory at the API boundary, matching Node.js behavior. These tests
// compare output against a reference encryption performed with unmodified buffers.

// Helper: encrypt with chacha20-poly1305 (AEAD path) and return {ciphertext, tag}.
function chachaEncrypt(key, iv, plaintext, aad) {
  const cipher = createCipheriv('chacha20-poly1305', key, iv);
  if (aad) cipher.setAAD(aad);
  const ct = cipher.update(plaintext);
  cipher.final();
  return { ciphertext: ct, tag: cipher.getAuthTag() };
}

// Helper: encrypt with aes-256-gcm (CipherHandle path) and return {ciphertext, tag}.
function gcmEncrypt(key, iv, plaintext) {
  const cipher = createCipheriv('aes-256-gcm', key, iv);
  const ct = cipher.update(plaintext);
  cipher.final();
  return { ciphertext: ct, tag: cipher.getAuthTag() };
}

export const modifiedIvChaCha20 = {
  test() {
    // ChaCha20-Poly1305 uses the AEAD (EVP_AEAD) path.
    const plainKey = Buffer.alloc(32, 0x01);
    const plainIv = Buffer.alloc(12, 0x42);
    const key = createSecretKey(plainKey);
    const plaintext = Buffer.from('hello world');

    // Reference encryption with plain buffers.
    const ref = chachaEncrypt(key, plainIv, plaintext);

    // Modify IV buffer after cipher creation -- must not affect output.
    const ivBuffer = new ArrayBuffer(12, { maxByteLength: 16 });
    new Uint8Array(ivBuffer).fill(0x42);
    const cipher = createCipheriv(
      'chacha20-poly1305',
      key,
      new Uint8Array(ivBuffer)
    );
    ivBuffer.resize(0);
    const ct = cipher.update(plaintext);
    cipher.final();

    deepStrictEqual(ct, ref.ciphertext);
    deepStrictEqual(cipher.getAuthTag(), ref.tag);
  },
};

export const modifiedIvAesGcm = {
  test() {
    // AES-256-GCM uses the CipherHandle (EVP_CIPHER) path.
    const plainKey = Buffer.alloc(32, 0x01);
    const plainIv = Buffer.alloc(16, 0x42);
    const key = createSecretKey(plainKey);
    const plaintext = Buffer.from('hello world');

    const ref = gcmEncrypt(key, plainIv, plaintext);

    const ivBuffer = new ArrayBuffer(16, { maxByteLength: 32 });
    new Uint8Array(ivBuffer).fill(0x42);
    const cipher = createCipheriv('aes-256-gcm', key, new Uint8Array(ivBuffer));
    ivBuffer.resize(0);
    const ct = cipher.update(plaintext);
    cipher.final();

    deepStrictEqual(ct, ref.ciphertext);
    deepStrictEqual(cipher.getAuthTag(), ref.tag);
  },
};

export const modifiedIvGrowChaCha20 = {
  test() {
    // Growing the IV buffer after cipher creation must not change the output.
    const plainKey = Buffer.alloc(32, 0x01);
    const plainIv = Buffer.alloc(12, 0x42);
    const key = createSecretKey(plainKey);
    const plaintext = Buffer.from('hello world');

    const ref = chachaEncrypt(key, plainIv, plaintext);

    const ivBuffer = new ArrayBuffer(12, { maxByteLength: 32 });
    new Uint8Array(ivBuffer).fill(0x42);
    const cipher = createCipheriv(
      'chacha20-poly1305',
      key,
      new Uint8Array(ivBuffer)
    );
    ivBuffer.resize(32); // grow past original IV length
    const ct = cipher.update(plaintext);
    cipher.final();

    deepStrictEqual(ct, ref.ciphertext);
    deepStrictEqual(cipher.getAuthTag(), ref.tag);
  },
};

export const modifiedAadChaCha20 = {
  test() {
    // Modifying AAD buffer after setAAD must not change the cipher output.
    const plainKey = Buffer.alloc(32, 0x01);
    const plainIv = Buffer.alloc(12, 0x42);
    const plainAad = Buffer.alloc(16, 0xaa);
    const key = createSecretKey(plainKey);
    const plaintext = Buffer.from('hello world');

    const ref = chachaEncrypt(key, plainIv, plaintext, plainAad);

    const aadBuffer = new ArrayBuffer(16, { maxByteLength: 32 });
    new Uint8Array(aadBuffer).fill(0xaa);
    const cipher = createCipheriv('chacha20-poly1305', key, plainIv);
    cipher.setAAD(new Uint8Array(aadBuffer));
    aadBuffer.resize(0);
    const ct = cipher.update(plaintext);
    cipher.final();

    deepStrictEqual(ct, ref.ciphertext);
    deepStrictEqual(cipher.getAuthTag(), ref.tag);
  },
};

export const modifiedAuthTagDecrypt = {
  test() {
    // Modifying auth tag buffer after setAuthTag must still allow correct decryption.
    const plainKey = Buffer.alloc(32, 0x01);
    const plainIv = Buffer.alloc(12, 0x42);
    const key = createSecretKey(plainKey);
    const plaintext = Buffer.from('hello world');

    const { ciphertext, tag } = chachaEncrypt(key, plainIv, plaintext);

    // Copy tag into a buffer, then modify it after setAuthTag.
    const tagBuffer = new ArrayBuffer(tag.length, { maxByteLength: 32 });
    new Uint8Array(tagBuffer).set(tag);

    const decipher = createDecipheriv('chacha20-poly1305', key, plainIv);
    decipher.setAuthTag(new Uint8Array(tagBuffer));
    tagBuffer.resize(0);

    const pt = decipher.update(ciphertext);
    decipher.final();
    deepStrictEqual(pt, plaintext);
  },
};

export const transferredIvChaCha20 = {
  test() {
    // Transferring IV buffer after cipher creation must not affect output.
    const plainKey = Buffer.alloc(32, 0x01);
    const plainIv = Buffer.alloc(12, 0x42);
    const key = createSecretKey(plainKey);
    const plaintext = Buffer.from('hello world');

    const ref = chachaEncrypt(key, plainIv, plaintext);

    const ivBuffer = new ArrayBuffer(12);
    new Uint8Array(ivBuffer).fill(0x42);
    const cipher = createCipheriv(
      'chacha20-poly1305',
      key,
      new Uint8Array(ivBuffer)
    );
    structuredClone(ivBuffer, { transfer: [ivBuffer] });
    const ct = cipher.update(plaintext);
    cipher.final();

    deepStrictEqual(ct, ref.ciphertext);
    deepStrictEqual(cipher.getAuthTag(), ref.tag);
  },
};

export const transferredIvAesGcm = {
  test() {
    const plainKey = Buffer.alloc(32, 0x01);
    const plainIv = Buffer.alloc(16, 0x42);
    const key = createSecretKey(plainKey);
    const plaintext = Buffer.from('hello world');

    const ref = gcmEncrypt(key, plainIv, plaintext);

    const ivBuffer = new ArrayBuffer(16);
    new Uint8Array(ivBuffer).fill(0x42);
    const cipher = createCipheriv('aes-256-gcm', key, new Uint8Array(ivBuffer));
    structuredClone(ivBuffer, { transfer: [ivBuffer] });
    const ct = cipher.update(plaintext);
    cipher.final();

    deepStrictEqual(ct, ref.ciphertext);
    deepStrictEqual(cipher.getAuthTag(), ref.tag);
  },
};

export const transferredAadChaCha20 = {
  test() {
    const plainKey = Buffer.alloc(32, 0x01);
    const plainIv = Buffer.alloc(12, 0x42);
    const plainAad = Buffer.alloc(16, 0xaa);
    const key = createSecretKey(plainKey);
    const plaintext = Buffer.from('hello world');

    const ref = chachaEncrypt(key, plainIv, plaintext, plainAad);

    const aadBuffer = new ArrayBuffer(16);
    new Uint8Array(aadBuffer).fill(0xaa);
    const cipher = createCipheriv('chacha20-poly1305', key, plainIv);
    cipher.setAAD(new Uint8Array(aadBuffer));
    structuredClone(aadBuffer, { transfer: [aadBuffer] });
    const ct = cipher.update(plaintext);
    cipher.final();

    deepStrictEqual(ct, ref.ciphertext);
    deepStrictEqual(cipher.getAuthTag(), ref.tag);
  },
};

export const transferredAuthTagDecrypt = {
  test() {
    const plainKey = Buffer.alloc(32, 0x01);
    const plainIv = Buffer.alloc(12, 0x42);
    const key = createSecretKey(plainKey);
    const plaintext = Buffer.from('hello world');

    const { ciphertext, tag } = chachaEncrypt(key, plainIv, plaintext);

    const tagBuffer = new ArrayBuffer(tag.length);
    new Uint8Array(tagBuffer).set(tag);

    const decipher = createDecipheriv('chacha20-poly1305', key, plainIv);
    decipher.setAuthTag(new Uint8Array(tagBuffer));
    structuredClone(tagBuffer, { transfer: [tagBuffer] });

    const pt = decipher.update(ciphertext);
    decipher.final();
    deepStrictEqual(pt, plaintext);
  },
};

export const testUnimplemented = {
  async test() {
    strictEqual(typeof Cipher, 'function');
    strictEqual(typeof Decipher, 'function');
    strictEqual(typeof createCipher, 'function');
    strictEqual(typeof createDecipher, 'function');
  },
};
