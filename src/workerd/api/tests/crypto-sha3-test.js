// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects, strictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';

const vectors = [
  [
    'SHA3-256',
    'a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a',
    '3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532',
  ],
  [
    'SHA3-384',
    '0c63a75b845e4f7d01107d852e4c2485c51a50aaaa94fc61995e71bbee983a2ac3713831264adb47fb6bd1e058d5f004',
    'ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c2596da7cf0e49be4b298d88cea927ac7f539f1edf228376d25',
  ],
  [
    'SHA3-512',
    'a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26',
    'b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0',
  ],
];

export const sha3Digest = {
  async test(_controller, env) {
    for (const [name, empty, abc] of vectors) {
      for (const [input, expected] of [
        ['', empty],
        ['abc', abc],
      ]) {
        for (const algorithm of [name, { name: name.toLowerCase() }]) {
          const digest = crypto.subtle.digest(
            algorithm,
            new TextEncoder().encode(input)
          );
          if (env.ENABLED) {
            strictEqual(Buffer.from(await digest).toString('hex'), expected);
          } else {
            await rejects(digest, { name: 'NotSupportedError' });
          }
        }
      }
      if (env.ENABLED) {
        strictEqual(crypto.subtle.constructor.supports('digest', name), true);
        strictEqual(
          crypto.subtle.constructor.supports('digest', {
            name: name.toLowerCase(),
          }),
          true
        );
        strictEqual(
          crypto.subtle.constructor.supports('generateKey', name),
          false
        );
        strictEqual(
          crypto.subtle.constructor.supports('importKey', name),
          false
        );
      }
    }
  },
};

export const sha3DigestOnly = {
  async test(_controller, env) {
    const data = new Uint8Array([1, 2, 3]);
    const ec = await crypto.subtle.generateKey(
      { name: 'ECDSA', namedCurve: 'P-256' },
      false,
      ['sign']
    );
    for (const [hash] of vectors) {
      const hmac = { name: 'HMAC', hash };
      const rsa = {
        name: 'RSA-PSS',
        hash,
        modulusLength: 2048,
        publicExponent: new Uint8Array([1, 0, 1]),
      };
      for (const algorithm of [hmac, rsa]) {
        await rejects(crypto.subtle.generateKey(algorithm, false, ['sign']), {
          name: 'NotSupportedError',
        });
        if (env.ENABLED)
          strictEqual(
            crypto.subtle.constructor.supports('generateKey', algorithm),
            false
          );
      }
      await rejects(
        crypto.subtle.importKey('raw', data, hmac, false, ['sign']),
        { name: 'NotSupportedError' }
      );
      const sign = { name: 'ECDSA', hash };
      await rejects(crypto.subtle.sign(sign, ec.privateKey, data), {
        name: 'NotSupportedError',
      });
      if (env.ENABLED)
        strictEqual(crypto.subtle.constructor.supports('sign', sign), false);
      for (const name of ['HKDF', 'PBKDF2']) {
        const key = await crypto.subtle.importKey('raw', data, name, false, [
          'deriveBits',
        ]);
        const algorithm = { name, hash, salt: data, info: data, iterations: 1 };
        await rejects(crypto.subtle.deriveBits(algorithm, key, 256), {
          name: 'NotSupportedError',
        });
        if (env.ENABLED)
          strictEqual(
            crypto.subtle.constructor.supports('deriveBits', algorithm, 256),
            false
          );
      }
    }
  },
};
