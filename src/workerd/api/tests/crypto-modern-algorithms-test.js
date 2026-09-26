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
