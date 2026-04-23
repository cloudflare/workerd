// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// secp256k1 ECDSA — behaviour when the `secp256k1_ecdsa_curve` compatibility
// flag is NOT set (the default today). The curve is unrecognized by
// `lookupEllipticCurve` and every entry point rejects with the pre-existing
// "unrecognized or unimplemented EC curve" message.
//
// This is a regression guard: if someone accidentally unconditionally
// registers secp256k1 (bypassing the flag), or changes the error message,
// these assertions will catch it.

import { rejects, strictEqual, ok } from 'node:assert';

export const generateKeyRejected = {
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
          /Unrecognized or unimplemented EC curve/.test(err.message),
          `unexpected message: ${err.message}`
        );
        return true;
      }
    );
  },
};

export const importKeyRawRejected = {
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
          /Unrecognized or unimplemented EC curve/.test(err.message),
          `unexpected message: ${err.message}`
        );
        return true;
      }
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
        { name: 'ECDSA', namedCurve: 'secp256k1' },
        true,
        ['verify']
      ),
      (err) => {
        strictEqual(err.name, 'NotSupportedError');
        ok(
          /Unrecognized or unimplemented EC curve/.test(err.message),
          `unexpected message: ${err.message}`
        );
        return true;
      }
    );
  },
};
