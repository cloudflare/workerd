// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws, doesNotThrow } from 'node:assert';

const methods = ['count', 'countReset', 'time', 'timeLog', 'timeEnd'];

export const labelConversion = {
  test() {
    for (const method of methods) {
      let calls = 0;
      console[method]({
        toString() {
          calls++;
          return 'label';
        },
      });
      strictEqual(calls, 1, method);

      const error = new Error('boom');
      const throwing = {
        toString() {
          throw error;
        },
      };
      throws(() => console[method](throwing), error, method);
      throws(() => console[method](Symbol('label')), TypeError, method);

      doesNotThrow(() => console[method](), method);
      doesNotThrow(() => console[method](undefined), method);
      doesNotThrow(() => console[method]('label', { extra: 1 }), method);
      strictEqual(console[method].name, method);
    }
  },
};
