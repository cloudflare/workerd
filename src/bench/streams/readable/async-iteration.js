// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// for await over a stream, compared with read-loop.js's reader loop over the
// same sources, and breaking out of the loop, which cancels the stream.

import { CHUNK, enqueuedStream, expectEqual, pullStream } from 'helpers';

async function iterate(stream) {
  let count = 0;
  for await (const _ of stream) count++;
  return count;
}

export const asyncIteration = {
  bench(b) {
    for (const count of [16, 256]) {
      b.run(`enqueued in start() x${count}`, async () => {
        expectEqual(await iterate(enqueuedStream(count)), count, 'chunks');
      });
    }
    b.run('pull() x256', async () => {
      expectEqual(await iterate(pullStream(256)), 256, 'chunks');
    });
    // The break cancels the stream, which the source's cancel() records. The
    // source doesn't close: once it has called close(), C++ cancels without
    // running its cancel(), even with chunks still queued, where the
    // TypeScript implementation runs it, as the spec requires.
    b.run('break after the first of 16', async () => {
      let cancelled = false;
      const stream = new ReadableStream({
        start(controller) {
          for (let i = 0; i < 16; i++) controller.enqueue(CHUNK);
        },
        cancel() {
          cancelled = true;
        },
      });
      let count = 0;
      for await (const _ of stream) {
        count++;
        break;
      }
      expectEqual(count, 1, 'chunks');
      expectEqual(cancelled, true, 'cancelled');
    });
  },
};
