// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-18.
// Two concurrent BYOB readAtLeast(5) requests, then enqueue 8 bytes
// followed by 2 bytes. The first enqueue should partially satisfy
// the first read (5 bytes), buffer the remaining 3 bytes, and the
// second enqueue (2 bytes) should complete the second read (3+2=5).
export const concurrentByobReadAtLeastPartialEnqueue = {
  async test() {
    let ctrl;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
    });

    const reader = rs.getReader({ mode: 'byob' });

    // Two concurrent readAtLeast(5) requests.
    const p1 = reader.readAtLeast(5, new Uint8Array(5));
    const p2 = reader.readAtLeast(5, new Uint8Array(5));

    // Enqueue 8 bytes — should fulfill first read (5 bytes),
    // buffer remaining 3 bytes for second read.
    ctrl.enqueue(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]));

    // Enqueue 2 more bytes — combined with buffered 3, should
    // fulfill second read (5 bytes total).
    ctrl.enqueue(new Uint8Array([9, 10]));

    const [r1, r2] = await Promise.all([p1, p2]);

    deepStrictEqual(
      [...new Uint8Array(r1.value.buffer)],
      [1, 2, 3, 4, 5],
      'first read should get bytes 1-5'
    );
    deepStrictEqual(
      [...new Uint8Array(r2.value.buffer)],
      [6, 7, 8, 9, 10],
      'second read should get bytes 6-10'
    );
  },
};
