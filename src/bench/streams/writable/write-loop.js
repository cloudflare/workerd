// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A stream's whole life per call: construct it, write every chunk, and
// close it. The writer awaits each write, queues every write before
// awaiting the close, or waits for `ready` before each write, as a producer
// honoring backpressure does.

import { CHUNK, countingSink, expectEqual } from 'helpers';

async function awaitEachWrite(count, sinkOptions) {
  const sink = countingSink(sinkOptions);
  const writer = new WritableStream(sink).getWriter();
  for (let i = 0; i < count; i++) await writer.write(CHUNK);
  await writer.close();
  expectEqual(sink.writes, count, 'chunks written');
}

async function queueEveryWrite(count, sinkOptions) {
  const sink = countingSink(sinkOptions);
  const writer = new WritableStream(sink).getWriter();
  const writes = [];
  for (let i = 0; i < count; i++) writes.push(writer.write(CHUNK));
  await Promise.all([...writes, writer.close()]);
  expectEqual(sink.writes, count, 'chunks written');
}

async function awaitReady(count, highWaterMark) {
  const sink = countingSink({ async: true });
  const writer = new WritableStream(sink, { highWaterMark }).getWriter();
  for (let i = 0; i < count; i++) {
    await writer.ready;
    writer.write(CHUNK);
  }
  await writer.close();
  expectEqual(sink.writes, count, 'chunks written');
}

export const writeLoop = {
  bench(b) {
    for (const count of [16, 256]) {
      b.run(`await each write(), sync sink x${count}`, () =>
        awaitEachWrite(count)
      );
    }
    b.run('await each write(), async sink x256', () =>
      awaitEachWrite(256, { async: true })
    );
    for (const count of [16, 256]) {
      b.run(`queue every write(), sync sink x${count}`, () =>
        queueEveryWrite(count)
      );
    }
    b.run('queue every write(), async sink x256', () =>
      queueEveryWrite(256, { async: true })
    );
    for (const highWaterMark of [1, 16]) {
      b.run(`await ready, highWaterMark ${highWaterMark} x256`, () =>
        awaitReady(256, highWaterMark)
      );
    }
  },
};
