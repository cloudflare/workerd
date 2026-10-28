// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Ending a stream early: abort() an idle stream, and one with writes queued
// behind a sink that has not started yet, which the abort rejects.

import { CHUNK, expectEqual } from 'helpers';

export const abort = {
  bench(b) {
    b.run('abort() an idle stream', async () => {
      await new WritableStream().abort();
    });
    b.run('abort() with 16 queued writes', async () => {
      let started;
      let written = 0;
      const writer = new WritableStream(
        {
          start() {
            return new Promise((resolve) => {
              started = resolve;
            });
          },
          write() {
            written++;
          },
        },
        { highWaterMark: 16 }
      ).getWriter();
      let rejected = 0;
      const writes = [];
      for (let i = 0; i < 16; i++) {
        writes.push(
          writer.write(CHUNK).catch(() => {
            rejected++;
          })
        );
      }
      const aborted = writer.abort();
      started();
      await Promise.all([...writes, aborted]);
      expectEqual(written, 0, 'chunks written');
      expectEqual(rejected, 16, 'writes rejected');
    });
  },
};
