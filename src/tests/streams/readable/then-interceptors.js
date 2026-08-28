// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object.prototype.then pollution while a read settles: the streams
// machinery must neither invoke the interceptor as a thenable nor leak
// it into results.

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const thenGetterFireCountOnRead = {
  async test() {
    let fired = 0;
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader();
    try {
      Object.defineProperty(Object.prototype, 'then', {
        get() {
          fired++;
          return undefined;
        },
        configurable: true,
      });
      const read = reader.read();
      controller.enqueue('x');
      const r = await read;
      strictEqual(r.value, 'x');
      controller.close();
      await reader.closed;
    } finally {
      delete Object.prototype.then;
    }
    strictEqual('then' in {}, false, 'interceptor must be removed');
    // Counts measured in the wd-test harness context (see the transform
    // suite's thenGetterFireCount for the context-sensitivity note).
    strictEqual(fired, usingTsImpl ? 2 : 1);
  },
};
