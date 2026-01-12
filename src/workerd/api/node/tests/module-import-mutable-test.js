// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, ok, doesNotThrow } from 'node:assert';
import timersPromises from 'node:timers/promises';

export const testTimersPromisesMutable = {
  async test() {
    //const { default: timersPromises } = await import('node:timers/promises');
    const originalSetImmediate = timersPromises.setImmediate;
    ok(typeof originalSetImmediate === 'function');

    const patchedSetImmediate = async function patchedSetImmediate() {
      return 'patched';
    };

    doesNotThrow(() => {
      timersPromises.setImmediate = patchedSetImmediate;
    });

    strictEqual(timersPromises.setImmediate, patchedSetImmediate);
    strictEqual(await timersPromises.setImmediate(), 'patched');
    timersPromises.setImmediate = originalSetImmediate;
    strictEqual(timersPromises.setImmediate, originalSetImmediate);
  },
};
