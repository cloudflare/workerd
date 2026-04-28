// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { rejects } from 'node:assert';

const ECDSA = { name: 'ECDSA', namedCurve: 'secp256k1' };
const expectedError = {
  name: 'NotSupportedError',
  message: /Unrecognized or unimplemented EC curve/,
};

export const generateKeyRejected = {
  async test() {
    await rejects(
      crypto.subtle.generateKey(ECDSA, true, ['sign', 'verify']),
      expectedError
    );
  },
};

export const importKeyRawRejected = {
  async test() {
    await rejects(
      crypto.subtle.importKey('raw', new Uint8Array(33), ECDSA, true, [
        'verify',
      ]),
      expectedError
    );
  },
};

export const importKeyJwkRejected = {
  async test() {
    await rejects(
      crypto.subtle.importKey(
        'jwk',
        {
          kty: 'EC',
          crv: 'secp256k1',
          x: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',
          y: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',
        },
        ECDSA,
        true,
        ['verify']
      ),
      expectedError
    );
  },
};
