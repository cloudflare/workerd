// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tee() of a byte stream, both branches read concurrently with default
// readers.

import {
  SHAPES,
  drainBytes,
  enqueuedByteStream,
  expectEqual,
  shapeLabel,
} from 'helpers';

export const tee = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(`read both branches ${shapeLabel(count, size)}`, async () => {
        const [left, right] = enqueuedByteStream(count, size).tee();
        const [a, c] = await Promise.all([
          drainBytes(left.getReader()),
          drainBytes(right.getReader()),
        ]);
        expectEqual(a, count * size, 'bytes read from the first branch');
        expectEqual(c, count * size, 'bytes read from the second branch');
      });
    }
  },
};
