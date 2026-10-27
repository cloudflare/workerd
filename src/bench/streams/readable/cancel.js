// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Ending a stream early: cancel() with and without a pending read or queued
// chunks, and the source erroring a pending read.

import { CHUNK, expectEqual } from 'helpers';

export const cancel = {
  bench(b) {
    b.run('cancel() an idle stream', async () => {
      await new ReadableStream().cancel();
    });
    b.run('cancel() with a pending read', async () => {
      const reader = new ReadableStream().getReader();
      const read = reader.read();
      await reader.cancel();
      expectEqual((await read).done, true, 'done');
    });
    b.run('cancel() discarding 256 queued chunks', async () => {
      const stream = new ReadableStream(
        {
          start(controller) {
            for (let i = 0; i < 256; i++) controller.enqueue(CHUNK);
          },
        },
        { highWaterMark: 256 }
      );
      await stream.cancel();
    });
    b.run('error() with a pending read', async () => {
      let controller;
      const reader = new ReadableStream({
        start(c) {
          controller = c;
        },
      }).getReader();
      const read = reader.read();
      const reason = new Error('boom');
      controller.error(reason);
      await read.then(
        () => {
          throw new Error('The read did not reject.');
        },
        (e) => expectEqual(e, reason, 'rejection')
      );
    });
  },
};
