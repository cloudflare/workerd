// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// secp256k1 ECDSA — behaviour when the `secp256k1_ecdsa_curve` compatibility
// flag is enabled. The curve is dispatched through libsecp256k1 (BoringSSL
// does not implement secp256k1).
//
// Current scope: `importKey("raw", publicKey)` and `verify(signature, data)`.
// Other operations (generateKey, sign, JWK import/export, SPKI, PKCS8) are
// not yet wired to the libsecp256k1 backend; tests for those assert the
// explicit "not yet implemented" rejections so the stubs can't regress
// silently.
//
// When sign/generate/JWK lands, flip the matching asserts from "must fail"
// to "must succeed" and add end-to-end round-trip tests.

import { rejects, strictEqual, ok } from 'node:assert';

// -----------------------------------------------------------------------
// Known-good secp256k1 public key for positive tests.
//
// 33-byte SEC1 compressed encoding of the secp256k1 generator point G
// (i.e. the public key for private key d = 1). This is not secret —
// it's literally the curve's generator. It's a stable, well-documented
// point that lets us exercise the import path without needing to ship
// test-only signing machinery.
// -----------------------------------------------------------------------
const GENERATOR_PUBKEY_COMPRESSED = new Uint8Array([
  0x02, 0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0, 0x62, 0x95,
  0xce, 0x87, 0x0b, 0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9, 0x59,
  0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98,
]);

// -----------------------------------------------------------------------
// importKey — supported paths
// -----------------------------------------------------------------------

export const importRawCompressedPublicKey = {
  async test() {
    const key = await crypto.subtle.importKey(
      'raw',
      GENERATOR_PUBKEY_COMPRESSED,
      { name: 'ECDSA', namedCurve: 'secp256k1' },
      true,
      ['verify']
    );
    strictEqual(key.type, 'public');
    strictEqual(key.algorithm.name, 'ECDSA');
    strictEqual(key.algorithm.namedCurve, 'secp256k1');
    // Usages should be restricted to 'verify' since this is a public key.
    ok(key.usages.includes('verify'));
    ok(!key.usages.includes('sign'));
  },
};

export const exportRawRoundTrip = {
  async test() {
    const key = await crypto.subtle.importKey(
      'raw',
      GENERATOR_PUBKEY_COMPRESSED,
      { name: 'ECDSA', namedCurve: 'secp256k1' },
      true,
      ['verify']
    );
    const exported = new Uint8Array(await crypto.subtle.exportKey('raw', key));
    strictEqual(exported.byteLength, 33);
    for (let i = 0; i < GENERATOR_PUBKEY_COMPRESSED.length; i++) {
      strictEqual(
        exported[i],
        GENERATOR_PUBKEY_COMPRESSED[i],
        `exported byte ${i} did not match input`
      );
    }
  },
};

export const importRejectsWrongLength = {
  async test() {
    // 32 bytes is not a valid SEC1 public key length.
    await rejects(
      crypto.subtle.importKey(
        'raw',
        new Uint8Array(32),
        { name: 'ECDSA', namedCurve: 'secp256k1' },
        true,
        ['verify']
      ),
      (err) => {
        strictEqual(err.name, 'DataError');
        return true;
      }
    );
  },
};

export const importRejectsInvalidPoint = {
  async test() {
    // 33 bytes with the right prefix but a garbage x-coordinate. The
    // prefix 0x02 says "compressed, y is even" but the payload won't
    // correspond to a point on secp256k1.
    const bogus = new Uint8Array(33);
    bogus[0] = 0x02;
    bogus.fill(0xff, 1);
    await rejects(
      crypto.subtle.importKey(
        'raw',
        bogus,
        { name: 'ECDSA', namedCurve: 'secp256k1' },
        true,
        ['verify']
      ),
      (err) => {
        strictEqual(err.name, 'DataError');
        return true;
      }
    );
  },
};

// -----------------------------------------------------------------------
// verify — negative cases
//
// Until sign() is wired up for secp256k1 we can't produce a signature
// inside the test, so we can't do a positive verify assertion here.
// What we CAN assert is that malformed / clearly-wrong signatures
// return false (not true, and not a thrown exception).
// -----------------------------------------------------------------------

export const verifyRejectsWrongLengthSignature = {
  async test() {
    const key = await crypto.subtle.importKey(
      'raw',
      GENERATOR_PUBKEY_COMPRESSED,
      { name: 'ECDSA', namedCurve: 'secp256k1' },
      true,
      ['verify']
    );
    // 63 bytes is not a valid ECDSA-raw signature (must be 64).
    const result = await crypto.subtle.verify(
      { name: 'ECDSA', hash: 'SHA-256' },
      key,
      new Uint8Array(63),
      new Uint8Array([0x00])
    );
    strictEqual(result, false);
  },
};

export const verifyRejectsAllZeroSignature = {
  async test() {
    const key = await crypto.subtle.importKey(
      'raw',
      GENERATOR_PUBKEY_COMPRESSED,
      { name: 'ECDSA', namedCurve: 'secp256k1' },
      true,
      ['verify']
    );
    // All-zero is not a valid signature (r = 0, s = 0 both fail the
    // parser's range check).
    const result = await crypto.subtle.verify(
      { name: 'ECDSA', hash: 'SHA-256' },
      key,
      new Uint8Array(64),
      new Uint8Array([0x00])
    );
    strictEqual(result, false);
  },
};

// -----------------------------------------------------------------------
// Paths NOT yet implemented — these MUST still reject until the
// follow-up PRs wire them in. If any of these starts passing, the
// error messages below need to be reviewed for consistency before the
// test is flipped to a positive assertion.
// -----------------------------------------------------------------------

export const generateKeyNotYetImplemented = {
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

export const importJwkNotYetImplemented = {
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
          /Key import format "jwk" is not yet implemented for secp256k1/.test(
            err.message
          ),
          `unexpected message: ${err.message}`
        );
        return true;
      }
    );
  },
};

export const ecdhRejectedExplicitly = {
  // The secp256k1 compat flag is scoped to ECDSA only. ECDH over
  // secp256k1 must fail with a specific error even when the flag is on.
  async test() {
    await rejects(
      crypto.subtle.importKey(
        'raw',
        GENERATOR_PUBKEY_COMPRESSED,
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
