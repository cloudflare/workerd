// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What an IdentityTransformStream queues for writes issued without
// awaiting: the writable's own queues and the chunk snapshots its write
// side takes at size() time. An array dequeued with shift() grew
// superlinearly on this shape past ~20k entries; the growth is milder
// than for the plain streams, since a write costs more than a dequeue,
// so the shape is timed over a 16x range. Every byte is pattern-checked.

import { strictEqual } from 'node:assert';
import { assertLinearScaling } from 'helpers';

export const identityUnawaitedWritesScaleLinearly = {
  async test() {
    await assertLinearScaling(
      async (n) => {
        const its = new IdentityTransformStream();
        const writer = its.writable.getWriter();
        const writes = [];
        for (let i = 0; i < n; i++) {
          writes.push(writer.write(new Uint8Array([i & 0xff])));
        }
        const closed = writer.close();
        const reader = its.readable.getReader();
        let total = 0;
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          for (let j = 0; j < value.byteLength; j++) {
            strictEqual(value[j], (total + j) & 0xff, `byte ${total + j}`);
          }
          total += value.byteLength;
        }
        strictEqual(total, n);
        await Promise.all(writes);
        await closed;
      },
      5_000,
      16
    );
  },
};
