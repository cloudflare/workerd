// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without fixup-transform-stream-backpressure the original bug applies:
// the readable-side backpressure latch is never set when an enqueue
// crosses the high-water mark (standard.c++, the flag site), so writes
// past a full readable queue complete immediately and the chunks flow.
// Because the latch never engages, this path has none of the raciness of
// the flag-on C++ path (see backpressure.js in the main cells).

import { strictEqual } from 'node:assert';

export const legacyNoBackpressureAtReadableHwm = {
  async test() {
    const ts = new TransformStream(
      {
        transform(chunk, c) {
          c.enqueue(chunk);
        },
      },
      { highWaterMark: 4 },
      { highWaterMark: 1 }
    );
    const writer = ts.writable.getWriter();

    // Both writes complete promptly even though the second enqueue
    // overfills the readable queue (hwm 1): the bug never latches.
    await writer.write('a');
    await writer.write('b');

    const reader = ts.readable.getReader();
    strictEqual((await reader.read()).value, 'a');
    strictEqual((await reader.read()).value, 'b');
    await writer.close();
    strictEqual((await reader.read()).done, true);
  },
};
