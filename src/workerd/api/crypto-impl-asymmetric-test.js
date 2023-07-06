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
