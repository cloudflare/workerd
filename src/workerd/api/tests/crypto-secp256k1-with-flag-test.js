// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// secp256k1 ECDSA — behaviour when the `secp256k1_ecdsa_curve` compatibility
// flag IS set but no backend is wired up yet. The curve is recognized by
// `lookupEllipticCurve` (so a different error message) but every entry point
// rejects with the stub "backend not available" message.
//
// When the backend lands, flip these assertions from "asserts failure" to
// "asserts success" (generate/import/sign/verify round-trips).

import { rejects, strictEqual, ok } from 'node:assert';

export const generateKeyStubs = {
  async test() {
    await rejects(
      crypto.subtle.generateKey(
        { name: 'ECDSA', namedCurve: 'secp256k1' },
        true,
        ['sign', 'verify']
      ),
      (err) => {
        strictEqual(err.name, 'NotSupportedError');
        ok(
          /Key generation for "secp256k1" is not yet implemented/.test(
            err.message
          ),
          `unexpected message: ${err.message}`
        );
        return true;
      }
    );
  },
};

export const importKeyJwkStubs = {
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
        { name: 'ECDSA', namedCurve: 'secp256k1' },
        true,
        ['verify']
      ),
      (err) => {
        strictEqual(err.name, 'NotSupportedError');
        ok(
          /Key import for "secp256k1" is not yet implemented/.test(err.message),
          `unexpected message: ${err.message}`
        );
        return true;
      }
    );
  },
};

export const importKeyRawStubs = {
  async test() {
    await rejects(
      crypto.subtle.importKey(
        'raw',
        new Uint8Array(33),
        { name: 'ECDSA', namedCurve: 'secp256k1' },
        true,
        ['verify']
      ),
      (err) => {
        strictEqual(err.name, 'NotSupportedError');
        ok(
          /Key import for "secp256k1" is not yet implemented/.test(err.message),
          `unexpected message: ${err.message}`
        );
        return true;
      }
    );
  },
};

export const ecdhRejectedExplicitly = {
  // The secp256k1 compat flag is scoped to ECDSA only. ECDH over secp256k1
  // must fail with a specific error even when the flag is on.
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
        { name: 'ECDH', namedCurve: 'secp256k1' },
        true,
        ['deriveBits']
      ),
      (err) => {
        strictEqual(err.name, 'NotSupportedError');
        ok(
          /ECDH is not supported for curve "secp256k1"/.test(err.message),
          `unexpected message: ${err.message}`
        );
        return true;
      }
    );
  },
};
