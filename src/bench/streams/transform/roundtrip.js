// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// One chunk through a long-lived transform stream at a time, so
// construction is out of the picture: each call writes a chunk and reads it
// back 16 times, since one round trip costs about as much as the runner's
// overhead per call. The streams never close, and each call leaves its
// stream as it found it.

import { CHUNK, TRANSFORMERS, expectEqual } from 'helpers';

const COUNT = 16;

export const roundtrip = {
  bench(b) {
    for (const name of [
      'no transformer',
      'transform() enqueues',
      'async transform()',
    ]) {
      const { readable, writable } = new TransformStream(
        TRANSFORMERS[name].make()
      );
      const writer = writable.getWriter();
      const reader = readable.getReader();
      b.run(`write() + read(), ${name} x${COUNT}`, async () => {
        for (let i = 0; i < COUNT; i++) {
          const [, { done }] = await Promise.all([
            writer.write(CHUNK),
            reader.read(),
          ]);
          expectEqual(done, false, 'done');
        }
      });
    }
  },
};
