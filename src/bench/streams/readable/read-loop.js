// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A stream's whole life per call: construct it, read it to the end with a
// default reader, and see it close. The counts separate the fixed cost from
// the per-chunk cost.

import {
  asyncPullStream,
  drainReader,
  enqueuedStream,
  expectEqual,
  pullStream,
} from 'helpers';

async function drain(stream, count) {
  expectEqual(await drainReader(stream.getReader()), count, 'chunks read');
}

export const readLoop = {
  bench(b) {
    for (const count of [1, 16, 256]) {
      b.run(`enqueued in start() x${count}`, () =>
        drain(enqueuedStream(count), count)
      );
    }
    for (const count of [16, 256]) {
      b.run(`pull() x${count}`, () => drain(pullStream(count), count));
    }
    // pull() runs ahead of the reads, keeping up to 16 chunks queued.
    b.run('pull(), highWaterMark 16 x256', () =>
      drain(pullStream(256, { highWaterMark: 16 }), 256)
    );
    for (const count of [16, 256]) {
      b.run(`async pull() x${count}`, () =>
        drain(asyncPullStream(count), count)
      );
    }
  },
};
