// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Reading a body twice: Response.clone() and body.tee() of runtime-produced
// bodies, and clone() of a body that is a JavaScript stream.

import {
  SHAPES,
  bytesOf,
  drainBytes,
  expectEqual,
  shapeLabel,
  sizeLabel,
  valueStream,
} from 'helpers';

async function readBoth(response, size) {
  const clone = response.clone();
  const [a, c] = await Promise.all([
    response.arrayBuffer(),
    clone.arrayBuffer(),
  ]);
  expectEqual(a.byteLength, size, 'bytes read from the original');
  expectEqual(c.byteLength, size, 'bytes read from the clone');
}

export const clone = {
  bench(b) {
    for (const size of [65536, 1048576]) {
      const bytes = bytesOf(size);
      b.run(`clone(), both read, ${sizeLabel(size)}`, () =>
        readBoth(new Response(bytes), size)
      );
    }
    for (const size of [65536, 1048576]) {
      const bytes = bytesOf(size);
      b.run(`body.tee(), both read, ${sizeLabel(size)}`, async () => {
        const [left, right] = new Response(bytes).body.tee();
        const [a, c] = await Promise.all([
          drainBytes(left.getReader()),
          drainBytes(right.getReader()),
        ]);
        expectEqual(a, size, 'bytes read from the first branch');
        expectEqual(c, size, 'bytes read from the second branch');
      });
    }
    const [count, size] = SHAPES[1];
    b.run(
      `clone() of a value stream body, both read, ${shapeLabel(count, size)}`,
      () => readBoth(new Response(valueStream(count, size)), count * size)
    );
  },
};
