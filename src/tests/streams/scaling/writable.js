// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The queues a writable stream keeps: the write requests and the
// controller's chunk queue, both filled by writes issued without
// awaiting and drained by a synchronous sink. An array dequeued with
// shift() grew 400x on this shape past ~20k entries. The sink checks
// every chunk's order.

import { strictEqual } from 'node:assert';
import { assertLinearScaling } from 'helpers';

export const unawaitedWritesScaleLinearly = {
  async test() {
    await assertLinearScaling(async (n) => {
      let received = 0;
      const ws = new WritableStream({
        write(chunk) {
          strictEqual(chunk, received, `write ${received} out of order`);
          received++;
        },
      });
      const writer = ws.getWriter();
      const writes = [];
      for (let i = 0; i < n; i++) writes.push(writer.write(i));
      await Promise.all(writes);
      await writer.close();
      strictEqual(received, n);
    }, 20_000);
  },
};
