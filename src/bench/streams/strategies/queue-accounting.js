// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What a strategy costs a stream: each enqueue() or write() sizes its chunk
// with the strategy's size(). A stream's whole life per call, compared with
// the same stream without a strategy.

import { CHUNK, expectEqual } from 'helpers';

// Under --quick, which only checks that each case runs, the cases queue a
// sixteenth as many chunks, so that the @smoke tests stay fast in
// sanitizer and debug builds.
const COUNT = 256;
const QUICK_COUNT = COUNT / 16;

// Each returns the strategy for a queue of `count` chunks.
const STRATEGIES = {
  'no strategy': () => undefined,
  CountQueuingStrategy: (count) =>
    new CountQueuingStrategy({ highWaterMark: count }),
  ByteLengthQueuingStrategy: (count) =>
    new ByteLengthQueuingStrategy({ highWaterMark: count * CHUNK.byteLength }),
  'size() function': (count) => ({ highWaterMark: count, size: () => 1 }),
};

export const queueAccounting = {
  bench(b) {
    const count = b.options.quick ? QUICK_COUNT : COUNT;
    for (const [name, make] of Object.entries(STRATEGIES)) {
      b.run(`ReadableStream, ${name} x${count}`, async () => {
        const reader = new ReadableStream(
          {
            start(controller) {
              for (let i = 0; i < count; i++) controller.enqueue(CHUNK);
              controller.close();
            },
          },
          make(count)
        ).getReader();
        let read = 0;
        while (!(await reader.read()).done) read++;
        expectEqual(read, count, 'chunks read');
      });
    }
    for (const [name, make] of Object.entries(STRATEGIES)) {
      b.run(`WritableStream, ${name} x${count}`, async () => {
        let written = 0;
        const writer = new WritableStream(
          {
            write() {
              written++;
            },
          },
          make(count)
        ).getWriter();
        const writes = [];
        for (let i = 0; i < count; i++) writes.push(writer.write(CHUNK));
        await Promise.all([...writes, writer.close()]);
        expectEqual(written, count, 'chunks written');
      });
    }
  },
};
