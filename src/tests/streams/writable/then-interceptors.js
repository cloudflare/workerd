// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Then-interceptor defense: writer.write()/close()/ready/closed all
// resolve with undefined, so settling them never performs a thenable
// lookup — a patched Object.prototype.then getter must NOT fire (parity;
// contrast the readable side, where read() results are plain objects and
// the identity/compression suites pin per-implementation fire counts).

import { strictEqual } from 'node:assert';

export const thenGetterDoesNotFireOnWriterPromises = {
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
      const ws = new WritableStream({
        write() {},
      });
      const writer = ws.getWriter();
      await writer.ready;
      await writer.write('x');
      await writer.close();
      await writer.closed;
    } finally {
      delete Object.prototype.then;
    }
    strictEqual('then' in {}, false, 'interceptor must be removed');
    strictEqual(fired, 0);
  },
};
