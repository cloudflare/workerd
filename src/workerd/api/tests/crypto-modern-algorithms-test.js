// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { rejects, strictEqual, throws } from 'node:assert';

export const supportsEcdhP521ByteRoundedDeriveBits = {
  async test() {
    const keyPair = await crypto.subtle.generateKey(
      { name: 'ECDH', namedCurve: 'P-521' },
      false,
      ['deriveBits']
    );
    const algorithm = { name: 'ECDH', public: keyPair.publicKey };

    const bits = await crypto.subtle.deriveBits(
      algorithm,
      keyPair.privateKey,
      528
    );
    strictEqual(bits.byteLength, 66);

    strictEqual(
      crypto.subtle.constructor.supports('deriveBits', algorithm, 528),
      true
    );
    strictEqual(
      crypto.subtle.constructor.supports('deriveBits', algorithm, 529),
      false
    );

    throws(
      () => crypto.subtle.constructor.supports('deriveBits', algorithm, -1),
      TypeError
    );
    throws(
      () =>
        crypto.subtle.constructor.supports('deriveBits', algorithm, 2 ** 32),
      TypeError
    );
  },
};

export const chacha20Poly1305 = {
  async test() {
    const key = await crypto.subtle.generateKey(
      { name: 'ChaCha20-Poly1305', length: 256 },
      true,
      ['encrypt', 'decrypt']
    );
    const iv = new Uint8Array(12);
    const plaintext = new TextEncoder().encode('hello');
    const ciphertext = await crypto.subtle.encrypt(
      { name: 'ChaCha20-Poly1305', iv },
      key,
      plaintext
    );
    const decrypted = await crypto.subtle.decrypt(
      { name: 'ChaCha20-Poly1305', iv },
      key,
      ciphertext
    );
    strictEqual(new TextDecoder().decode(decrypted), 'hello');

    const jwk = await crypto.subtle.exportKey('jwk', key);
    await crypto.subtle.importKey('jwk', jwk, 'ChaCha20-Poly1305', true, [
      'encrypt',
      'decrypt',
    ]);
    await rejects(
      crypto.subtle.importKey(
        'raw',
        new Uint8Array(32),
        'ChaCha20-Poly1305',
        true,
        ['encrypt']
      ),
      { name: 'NotSupportedError' }
    );
    await rejects(crypto.subtle.exportKey('raw', key), {
      name: 'NotSupportedError',
    });

    const baseKey = await crypto.subtle.importKey(
      'raw',
      new Uint8Array(32),
      'HKDF',
      false,
      ['deriveKey']
    );
    const derivedKey = await crypto.subtle.deriveKey(
      {
        name: 'HKDF',
        hash: 'SHA-256',
        salt: new Uint8Array(16),
        info: new Uint8Array(),
      },
      baseKey,
      'ChaCha20-Poly1305',
      true,
      ['encrypt']
    );
    strictEqual(
      (await crypto.subtle.exportKey('raw-secret', derivedKey)).byteLength,
      32
    );
  },
};

export const hybridKemMlkem768X25519 = {
  async test() {
    const keyPair = await crypto.subtle.generateKey('MLKEM768-X25519', true, [
      'encapsulateBits',
      'decapsulateBits',
    ]);
    const encapsulated = await crypto.subtle.encapsulateBits(
      'MLKEM768-X25519',
      keyPair.publicKey
    );
    const decapsulated = await crypto.subtle.decapsulateBits(
      'MLKEM768-X25519',
      keyPair.privateKey,
      encapsulated.ciphertext
    );
    strictEqual(
      new Uint8Array(encapsulated.sharedKey).toString(),
      new Uint8Array(decapsulated).toString()
    );

    const derivedPublicKey = await crypto.subtle.getPublicKey(
      keyPair.privateKey,
      ['encapsulateBits']
    );
    const publicJwk = await crypto.subtle.exportKey('jwk', keyPair.publicKey);
    const derivedPublicJwk = await crypto.subtle.exportKey(
      'jwk',
      derivedPublicKey
    );
    const privateJwk = await crypto.subtle.exportKey('jwk', keyPair.privateKey);
    strictEqual(derivedPublicJwk.pub, publicJwk.pub);
    await crypto.subtle.importKey('jwk', publicJwk, 'MLKEM768-X25519', true, [
      'encapsulateBits',
    ]);
    await crypto.subtle.importKey('jwk', privateJwk, 'MLKEM768-X25519', true, [
      'decapsulateBits',
    ]);
    await rejects(
      crypto.subtle.importKey(
        'jwk',
        {
          ...privateJwk,
          pub: `${privateJwk.pub[0] === 'A' ? 'B' : 'A'}${privateJwk.pub.slice(1)}`,
        },
        'MLKEM768-X25519',
        true,
        ['decapsulateBits']
      ),
      { name: 'DataError' }
    );
  },
};

export const mlDsaJwkKeyOpsValidation = {
  async test() {
    const keyPair = await crypto.subtle.generateKey('ML-DSA-44', true, [
      'sign',
    ]);
    const jwk = await crypto.subtle.exportKey('jwk', keyPair.privateKey);

    await rejects(
      crypto.subtle.importKey(
        'jwk',
        { ...jwk, key_ops: [] },
        'ML-DSA-44',
        true,
        ['sign']
      ),
      { name: 'DataError' }
    );

    await rejects(
      crypto.subtle.importKey(
        'jwk',
        { ...jwk, key_ops: ['sign', 'sign'] },
        'ML-DSA-44',
        true,
        ['sign']
      ),
      { name: 'DataError' }
    );

    const jwkWithoutAlg = { ...jwk };
    delete jwkWithoutAlg.alg;
    await rejects(
      crypto.subtle.importKey('jwk', jwkWithoutAlg, 'ML-DSA-44', true, [
        'sign',
      ]),
      { name: 'DataError' }
    );
  },
};

export const mlDsaContextLength = {
  async test() {
    const keyPair = await crypto.subtle.generateKey('ML-DSA-44', false, [
      'sign',
      'verify',
    ]);
    const data = new Uint8Array([1]);
    const context = new Uint8Array(256);
    const validAlgorithm = {
      name: 'ML-DSA-44',
      context: context.subarray(0, 255),
    };
    const invalidAlgorithm = { name: 'ML-DSA-44', context };

    const signature = await crypto.subtle.sign(
      validAlgorithm,
      keyPair.privateKey,
      data
    );
    strictEqual(
      await crypto.subtle.verify(
        validAlgorithm,
        keyPair.publicKey,
        signature,
        data
      ),
      true
    );
    await rejects(
      crypto.subtle.sign(invalidAlgorithm, keyPair.privateKey, data),
      {
        name: 'OperationError',
      }
    );
    await rejects(
      crypto.subtle.verify(
        invalidAlgorithm,
        keyPair.publicKey,
        signature,
        data
      ),
      { name: 'OperationError' }
    );
    for (const context of [null, 'invalid']) {
      await rejects(
        crypto.subtle.sign(
          { name: 'ML-DSA-44', context },
          keyPair.privateKey,
          data
        ),
        { name: 'TypeError' }
      );
    }
  },
};

export const hybridKemJwkMetadata = {
  async test() {
    const pair = await crypto.subtle.generateKey('MLKEM768-X25519', true, [
      'encapsulateBits',
      'decapsulateBits',
    ]);
    for (const [key, usage] of [
      [pair.publicKey, 'encapsulateBits'],
      [pair.privateKey, 'decapsulateBits'],
    ]) {
      const jwk = await crypto.subtle.exportKey('jwk', key);
      for (const metadata of [
        { use: 'sig' },
        { key_ops: [] },
        { key_ops: [usage, usage] },
        { ext: false },
      ]) {
        await rejects(
          crypto.subtle.importKey(
            'jwk',
            { ...jwk, ...metadata },
            'MLKEM768-X25519',
            true,
            [usage]
          ),
          { name: 'DataError' }
        );
      }
      const imported = await crypto.subtle.importKey(
        'jwk',
        { ...jwk, ext: false },
        'MLKEM768-X25519',
        false,
        [usage]
      );
      strictEqual(imported.extractable, false);
    }
  },
};

export const chacha20Poly1305Parameters = {
  async test() {
    const name = 'ChaCha20-Poly1305';
    const key = await crypto.subtle.generateKey({ name, length: 128 }, true, [
      'encrypt',
      'decrypt',
    ]);
    strictEqual(
      (await crypto.subtle.exportKey('raw-secret', key)).byteLength,
      32
    );
    strictEqual(
      crypto.subtle.constructor.supports('generateKey', { name, length: 128 }),
      true
    );
    const iv = new Uint8Array(12);
    const additionalData = new Uint8Array([1, 2, 3]);
    const algorithm = { name, iv, additionalData };
    const encrypted = await crypto.subtle.encrypt(
      algorithm,
      key,
      new Uint8Array()
    );
    strictEqual(encrypted.byteLength, 16);
    strictEqual(
      (await crypto.subtle.decrypt(algorithm, key, encrypted)).byteLength,
      0
    );
    for (const invalid of [
      { ...algorithm, iv: new Uint8Array(11) },
      { ...algorithm, tagLength: 96 },
    ]) {
      await rejects(crypto.subtle.encrypt(invalid, key, new Uint8Array()), {
        name: 'OperationError',
      });
      strictEqual(
        crypto.subtle.constructor.supports('encrypt', invalid),
        false
      );
    }
    strictEqual(
      (
        await crypto.subtle.encrypt(
          { ...algorithm, tagLength: 128.5 },
          key,
          new Uint8Array()
        )
      ).byteLength,
      16
    );
    for (const tagLength of [-1, 256, NaN, Infinity, -Infinity]) {
      await rejects(
        crypto.subtle.encrypt(
          { ...algorithm, tagLength },
          key,
          new Uint8Array()
        ),
        { name: 'TypeError' }
      );
    }
    await rejects(
      crypto.subtle.decrypt(
        { ...algorithm, additionalData: new Uint8Array([4]) },
        key,
        encrypted
      ),
      { name: 'OperationError' }
    );
    await rejects(crypto.subtle.decrypt(algorithm, key, new Uint8Array(15)), {
      name: 'OperationError',
    });
    const aesKey = await crypto.subtle.generateKey(
      { name: 'AES-GCM', length: 256 },
      false,
      ['encrypt']
    );
    for (const tagLength of [NaN, Infinity]) {
      await rejects(
        crypto.subtle.encrypt(
          { name: 'AES-GCM', iv, tagLength },
          aesKey,
          new Uint8Array()
        ),
        {
          name: 'OperationError',
        }
      );
    }
    strictEqual(
      (
        await crypto.subtle.encrypt(
          { name: 'AES-GCM', iv, tagLength: 96.5 },
          aesKey,
          new Uint8Array()
        )
      ).byteLength,
      12
    );
  },
};

export const hybridKemRawFormatsAndSharedKeys = {
  async test() {
    const name = 'MLKEM768-X25519';
    const pair = await crypto.subtle.generateKey(name, true, [
      'encapsulateKey',
      'decapsulateKey',
      'encapsulateBits',
      'decapsulateBits',
    ]);
    const seed = await crypto.subtle.exportKey('raw-seed', pair.privateKey);
    const pub = await crypto.subtle.exportKey('raw-public', pair.publicKey);
    strictEqual(seed.byteLength, 32);
    strictEqual(pub.byteLength, 1216);
    const privateKey = await crypto.subtle.importKey(
      'raw-seed',
      seed,
      name,
      false,
      ['decapsulateBits', 'decapsulateKey']
    );
    const publicKey = await crypto.subtle.importKey(
      'raw-public',
      pub,
      name,
      true,
      ['encapsulateBits', 'encapsulateKey']
    );
    for (const algorithm of [
      'ChaCha20-Poly1305',
      { name: 'AES-GCM', length: 256 },
    ]) {
      strictEqual(
        crypto.subtle.constructor.supports('encapsulateKey', name, algorithm),
        true
      );
      strictEqual(
        crypto.subtle.constructor.supports('decapsulateKey', name, algorithm),
        true
      );
      const result = await crypto.subtle.encapsulateKey(
        name,
        publicKey,
        algorithm,
        true,
        ['encrypt']
      );
      strictEqual(result.ciphertext.byteLength, 1120);
      const shared = await crypto.subtle.decapsulateKey(
        name,
        privateKey,
        result.ciphertext,
        algorithm,
        true,
        ['encrypt']
      );
      strictEqual(
        new Uint8Array(
          await crypto.subtle.exportKey('raw-secret', shared)
        ).toString(),
        new Uint8Array(
          await crypto.subtle.exportKey('raw-secret', result.sharedKey)
        ).toString()
      );
    }
    for (const size of [0, 1119, 1121]) {
      await rejects(
        crypto.subtle.decapsulateBits(name, privateKey, new Uint8Array(size)),
        { name: 'OperationError' }
      );
    }
    await rejects(
      crypto.subtle.decapsulateBits(name, privateKey, new Uint8Array(1120)),
      { name: 'OperationError' }
    );
    for (const [format, data, usages] of [
      ['raw-public', new Uint8Array(1215), ['encapsulateBits']],
      ['raw-seed', new Uint8Array(31), ['decapsulateBits']],
    ]) {
      await rejects(crypto.subtle.importKey(format, data, name, true, usages), {
        name: 'DataError',
      });
    }
  },
};
