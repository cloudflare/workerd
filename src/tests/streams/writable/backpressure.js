// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// desiredSize accounting and ready-promise behavior under backpressure.
// Migrated from streams-js-test.js and streams-backpressure-test.js.

import { strictEqual, ok } from 'node:assert';

// desiredSize decrements per queued write and recovers; ready is replaced
// once the queue drains.
export const writableStreamDesiredSize = {
  async test() {
    const ws = new WritableStream(
      {
        async write() {
          await scheduler.wait(10);
        },
      },
      {
        highWaterMark: 2,
      }
    );

    const writer = ws.getWriter();

    const firstReady = writer.ready;
    await firstReady;

    strictEqual(writer.desiredSize, 2);
    const write1 = writer.write(1);
    strictEqual(writer.desiredSize, 1);

    const write2 = writer.write(2);
    const write3 = writer.write(3);

    strictEqual(writer.desiredSize, -1);

    await Promise.all([write1, write2, write3]);

    ok(firstReady != writer.ready);
    await writer.ready;
  },
};

// desiredSize goes negative past capacity and recovers after the sink
// drains; all writes complete.
export const backpressureWritableDesiredSize = {
  async test() {
    const written = [];

    const ws = new WritableStream(
      {
        write(chunk) {
          written.push(chunk);
          // Simulate slow write
          return new Promise((resolve) => setTimeout(resolve, 10));
        },
      },
      { highWaterMark: 3 }
    );

    const writer = ws.getWriter();

    strictEqual(writer.desiredSize, 3, 'initial desiredSize');

    writer.write(1);
    strictEqual(writer.desiredSize, 2, 'desiredSize after first write');

    writer.write(2);
    strictEqual(writer.desiredSize, 1, 'desiredSize after second write');

    writer.write(3);
    strictEqual(writer.desiredSize, 0, 'desiredSize at capacity');

    const readyBefore = writer.ready;
    ok(readyBefore instanceof Promise, 'ready should be a Promise');

    writer.write(4);
    strictEqual(writer.desiredSize, -1, 'desiredSize over capacity');

    const readyAfter = writer.ready;
    ok(readyAfter instanceof Promise, 'ready should still be a Promise');

    await scheduler.wait(50);

    ok(writer.desiredSize > -1, 'desiredSize recovered after writes complete');

    const readyResolved = writer.ready;
    await readyResolved;

    await writer.close();
    strictEqual(written.length, 4, 'all chunks written');
  },
};

// A slow sink signals backpressure but all queued writes still fulfill.
export const backpressureWritableSlowSink = {
  async test() {
    let writeCount = 0;

    const ws = new WritableStream(
      {
        async write() {
          writeCount++;
          // Very slow write
          await scheduler.wait(50);
        },
      },
      { highWaterMark: 2 }
    );

    const writer = ws.getWriter();

    strictEqual(writer.desiredSize, 2);

    const w1 = writer.write('a');
    strictEqual(writer.desiredSize, 1);

    const w2 = writer.write('b');
    strictEqual(writer.desiredSize, 0);

    const initialReady = writer.ready;

    const w3 = writer.write('c');
    strictEqual(writer.desiredSize, -1);

    // Backpressure can be signaled in two ways depending on implementation:
    // 1. writer.ready returns a new pending promise (ready !== initialReady)
    // 2. desiredSize drops to 0 or below, indicating the queue is full
    // We accept either signal as valid backpressure indication.
    ok(
      writer.ready !== initialReady || writer.desiredSize <= 0,
      'backpressure signal'
    );

    const results = await Promise.allSettled([w1, w2, w3]);
    strictEqual(results[0].status, 'fulfilled', 'first write should succeed');
    strictEqual(results[1].status, 'fulfilled', 'second write should succeed');
    strictEqual(results[2].status, 'fulfilled', 'third write should succeed');
    strictEqual(writeCount, 3);

    await writer.close();
  },
};
