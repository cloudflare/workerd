// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What a strategy costs a stream: each enqueue() or write() sizes its chunk
// with the strategy's size(). A stream's whole life per call, compared with
// the same stream without a strategy.

import { CHUNK, expectEqual } from 'helpers';

const COUNT = 256;

const STRATEGIES = {
  'no strategy': () => undefined,
  CountQueuingStrategy: () =>
    new CountQueuingStrategy({ highWaterMark: COUNT }),
  ByteLengthQueuingStrategy: () =>
    new ByteLengthQueuingStrategy({ highWaterMark: COUNT * CHUNK.byteLength }),
  'size() function': () => ({ highWaterMark: COUNT, size: () => 1 }),
};

export const queueAccounting = {
  bench(b) {
    for (const [name, make] of Object.entries(STRATEGIES)) {
      b.run(`ReadableStream, ${name} x${COUNT}`, async () => {
        const reader = new ReadableStream(
          {
            start(controller) {
              for (let i = 0; i < COUNT; i++) controller.enqueue(CHUNK);
              controller.close();
            },
          },
          make()
        ).getReader();
        let read = 0;
        while (!(await reader.read()).done) read++;
        expectEqual(read, COUNT, 'chunks read');
      });
    }
    for (const [name, make] of Object.entries(STRATEGIES)) {
      b.run(`WritableStream, ${name} x${COUNT}`, async () => {
        let written = 0;
        const writer = new WritableStream(
          {
            write() {
              written++;
            },
          },
          make()
        ).getWriter();
        const writes = [];
        for (let i = 0; i < COUNT; i++) writes.push(writer.write(CHUNK));
        await Promise.all([...writes, writer.close()]);
        expectEqual(written, COUNT, 'chunks written');
      });
    }
  },
};
