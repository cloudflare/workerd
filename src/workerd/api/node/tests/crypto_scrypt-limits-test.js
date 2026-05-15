// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws } from 'node:assert';
import { scrypt, scryptSync } from 'node:crypto';

export const scrypt_cost_limit_sync = {
  test() {
    // N=1024, r=1, p=32768 → cost=33,554,432, exceeds 2^20
    throws(() => scryptSync('p', 's', 64, { N: 1024, r: 1, p: 32768 }), {
      name: 'RangeError',
    });

    // N=2, r=1, p=1048577 → cost just over 2^20
    throws(() => scryptSync('p', 's', 64, { N: 2, r: 1, p: 1048577 }), {
      name: 'RangeError',
    });

    // N=1024, r=1, p=1 → cost=1024, within limit
    const result = scryptSync('p', 's', 64, { N: 1024, r: 1, p: 1 });
    strictEqual(result.length, 64);
  },
};

export const scrypt_cost_limit_async = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();
    scrypt('p', 's', 64, { N: 1024, r: 1, p: 32768 }, (err) => {
      if (err) {
        resolve(err);
      } else {
        reject(new Error('Expected error'));
      }
    });
    const err = await promise;
    strictEqual(err.constructor.name, 'RangeError');
  },
};
