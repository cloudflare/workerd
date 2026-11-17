// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects, strictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';

// Vectors from WPT WebCryptoAPI/sign_verify/eddsa_vectors.js (kSmallOrderTestCases), which come
// from https://eprint.iacr.org/2020/1244.pdf. Each signature is accepted by Ed25519 verification
// without a small-order check.
const smallOrderVectors = {
  // A and R are both of small order, S = 0.
  smallKeyAndR: {
    key: 'c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa',
    sig: 'c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a0000000000000000000000000000000000000000000000000000000000000000',
    msg: '8c93255d71dcab10e8f379c26200f3c7bd5f09d9bc3068d3ef4edeb4853022b6',
  },
  // A is of small order.
  smallKey: {
    key: 'c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa',
    sig: 'f7badec5b8abeaf699583992219b7b223f1df3fbbea919844e3f7c554a43dd43a5bb704786be79fc476f91d3f3f89b03984d8068dcf1bb7dfc6637b45450ac04',
    msg: '9bd9f44f4dcc75bd531b56b2cd280b0bb38fc1cd6d1230e14861d861de092e79',
  },
  // R is of small order.
  smallR: {
    key: 'f7badec5b8abeaf699583992219b7b223f1df3fbbea919844e3f7c554a43dd43',
    sig: 'c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa8c4bd45aecaca5b24fb97bc10ac27ac8751a7dfe1baff8b953ec9f5833ca260e',
    msg: 'aebf3f2601a0c8c5d39cc7d8911642f740b78168218da8471772b35f9d35b9ab',
  },
};

// A and R of mixed order: not small, so this signature verifies either way.
const mixedOrderVector = {
  key: 'cdb267ce40c5cd45306fa5d2f29731459387dbf9eb933b7bd5aed9a765b88d4d',
  sig: '9046a64750444938de19f227bb80485e92b83fdb4b6506c160484c016cc1852f87909e14428a7a1d62e9f22f3d3ad7802db02eb2e688b6c52fcd6648a98bd009',
  msg: '9bd9f44f4dcc75bd531b56b2cd280b0bb38fc1cd6d1230e14861d861de092e79',
};

function hex(s) {
  return Buffer.from(s, 'hex');
}

async function verify(vector, name = 'Ed25519') {
  const algorithm = name === 'NODE-ED25519' ? { name, namedCurve: name } : name;
  const key = await crypto.subtle.importKey(
    'raw',
    hex(vector.key),
    algorithm,
    false,
    ['verify']
  );
  return crypto.subtle.verify(algorithm, key, hex(vector.sig), hex(vector.msg));
}

export const smallOrderPoints = {
  async test(ctrl, env) {
    for (const [name, vector] of Object.entries(smallOrderVectors)) {
      for (const algorithm of ['Ed25519', 'NODE-ED25519']) {
        strictEqual(
          await verify(vector, algorithm),
          !env.SPEC_VERIFY,
          `${name} with ${algorithm}`
        );
      }
    }
    strictEqual(await verify(mixedOrderVector), true);
  },
};

export const wrongSignatureLength = {
  async test(ctrl, env) {
    const { publicKey, privateKey } = await crypto.subtle.generateKey(
      'Ed25519',
      false,
      ['sign', 'verify']
    );
    const data = new TextEncoder().encode('hello');
    const signature = new Uint8Array(
      await crypto.subtle.sign('Ed25519', privateKey, data)
    );
    strictEqual(
      await crypto.subtle.verify('Ed25519', publicKey, signature, data),
      true
    );

    for (const bad of [
      signature.subarray(1),
      new Uint8Array([...signature, 0]),
      new Uint8Array(0),
    ]) {
      const result = crypto.subtle.verify('Ed25519', publicKey, bad, data);
      if (env.SPEC_VERIFY) {
        strictEqual(await result, false, `length ${bad.length}`);
      } else {
        await rejects(result, {
          name: 'OperationError',
          message: `Invalid Ed25519 signature length ${bad.length}`,
        });
      }
    }
  },
};
