// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { rejects, strictEqual } from 'node:assert';

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
