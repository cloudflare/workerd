// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Then-interceptor defense: unlike the writable suite (whose promises
// all resolve with undefined and never do a thenable lookup), the
// transform's READ side resolves read() promises with fresh plain
// `{done, value}` objects, so a patched Object.prototype.then getter DOES
// fire while they settle. The exact count differs per implementation's
// internal promise plumbing.

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const thenGetterFireCount = {
  async test() {
    let fired = 0;
    Object.defineProperty(Object.prototype, 'then', {
      get() {
        fired++;
        return undefined;
      },
      configurable: true,
    });
    try {
      const ts = new TransformStream({
        transform(chunk, c) {
          c.enqueue(chunk);
        },
      });
      const writer = ts.writable.getWriter();
      const reader = ts.readable.getReader();
      await Promise.allSettled([writer.write('x'), reader.read()]);
      await writer.close();
    } finally {
      delete Object.prototype.then;
    }
    strictEqual('then' in {}, false, 'interceptor must be removed');
    // Counts measured in the wd-test harness context; a fetch-handler
    // context adds exactly one more fire on each side (Response
    // plumbing), preserving the one-fire TypeScript/C++ delta.
    strictEqual(fired, usingTsImpl ? 3 : 2);
  },
};
