// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const rsa_reject_infinite_loop_test = {
  async test(ctrl, env, ctx) {
    // Check that parameters that may cause an infinite loop in RSA key generation are rejected.
    await assert.rejects(
      crypto.subtle.generateKey(
        {
          name: "RSASSA-PKCS1-v1_5",
          hash: "SHA-256",
          modulusLength: 1024,
          publicExponent: new Uint8Array([1]),
        },
        false,
        ["sign", "verify"]
      ),
      { message: 'The "publicExponent" must be either 3 or 65537, but got 1.' }
    );

    await assert.rejects(
      crypto.subtle.generateKey(
        {
          name: "RSA-PSS",
          hash: "SHA-256",
          modulusLength: 1024,
          publicExponent: new Uint8Array([1]),
        },
        false,
        ["sign", "verify"]
      ),
      { message: 'The "publicExponent" must be either 3 or 65537, but got 1.', }
    );
  }
}

export const eddsa_test = {
  async test(ctrl, env, ctx) {
    // Test EDDSA ED25519 generateKey
    const edKey = await crypto.subtle.generateKey(
      {
        name: "NODE-ED25519",
        namedCurve: "NODE-ED25519",
      },
      false,
      ["sign", "verify"]
    );
    assert.ok(edKey.publicKey.algorithm.name == "NODE-ED25519");
  }
}

export const publicExponent_type_test = {
  async test(ctrl, env, ctx) {
    const key = await crypto.subtle.generateKey(
      {
        name: "RSA-PSS",
        hash: "SHA-256",
        modulusLength: 1024,
        publicExponent: new Uint8Array([0x01, 0x00, 0x01]),
      },
      false,
      ["sign", "verify"]
    );

    // Check that a Uint8Array is used for publicExponent. Without the
    // crypto_preserve_public_exponent feature flag, this would incorrectly return an ArrayBuffer.
    assert.ok(key.publicKey.algorithm.publicExponent[Symbol.toStringTag] == "Uint8Array");
  }
}

export const ecdhJwkTest = {
  async test() {
    const publicJwk = {
      kty: 'EC',
      crv: 'P-256',
      alg: 'THIS CAN BE ANYTHING',
      x: 'Ze2loSV3wrroKUN_4zhwGhCqo3Xhu1td4QjeQ5wIVR0',
      y: 'HlLtdXARY_f55A3fnzQbPcm6hgr34Mp8p-nuzQCE0Zw',
    }

    // The import should succeed with no errors.
    // Refs: https://github.com/cloudflare/workerd/issues/1403
    await crypto.subtle.importKey('jwk', publicJwk,
                                  { name: 'ECDH', namedCurve: 'P-256' }, true, []);
  }
};
