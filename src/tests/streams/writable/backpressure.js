// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// desiredSize accounting and ready-promise behavior under backpressure.
// Migrated from streams-js-test.js and streams-backpressure-test.js, plus
// the floating-point queue-total pins mirroring WPT
// writable-streams/floating-point-total-queue-size.any.js.
//
// DIVERGENCE (queue arithmetic): the spec tracks [[queueTotalSize]] as a
// double. The C++ implementation converts each size() result to uint64
// through the jsg boundary (fractions truncate toward zero; negatives,
// NaN and infinities throw TypeError — common.h StreamQueuingStrategy)
// and then narrows the ssize_t desiredSize through `int`, so totals past
// 2^31 wrap (WritableStreamJsController::getDesiredSize). These are the
// three WPT floating-point expectedFailures. TypeScript follows the spec
// exactly, including RangeError("Invalid chunk size") for invalid size
// returns.

import { strictEqual, ok, rejects } from 'node:assert';
import { usingTsImpl, pedanticWpt } from 'which-impl';

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

// Builds the WPT floating-point scenario: identity size(), highWaterMark
// 0, and a gated sink so queued chunks stay queued while desiredSize is
// read.
function gatedFpStream() {
  let release;
  const gate = new Promise((res) => (release = res));
  const ws = new WritableStream(
    {
      async write() {
        await gate;
      },
    },
    {
      size(x) {
        return x;
      },
      highWaterMark: 0,
    }
  );
  return { writer: ws.getWriter(), release };
}

// The four WPT floating-point queue-total scenarios, with each side's
// exact arithmetic pinned.
export const floatingPointQueueTotals = {
  async test() {
    // Chunk sizes, expected desiredSize while queued (ts: double math;
    // cpp: uint64-truncated sizes, so 1e-16 and 2e-16 count as 0), and
    // expected desiredSize after the queue drains.
    const cases = [
      {
        sizes: [2, Number.MAX_SAFE_INTEGER],
        // ts: -(2 + MAX_SAFE_INTEGER) rounded in double arithmetic; cpp:
        // the true total exceeds 2^31 and the int narrowing wraps to -1.
        during: usingTsImpl ? 0 - 2 - Number.MAX_SAFE_INTEGER : -1,
        after: 0,
      },
      {
        sizes: [1e-16, 1],
        during: usingTsImpl ? 0 - 1e-16 - 1 : -1,
        after: 0,
      },
      {
        sizes: [1e-16, 1, 2e-16],
        during: usingTsImpl ? 0 - 1e-16 - 1 - 2e-16 : -1,
        // ts: the spec's incremental subtraction leaves a residue that is
        // not clamped (the WPT "positive, and not clamped" case).
        after: usingTsImpl ? 0 - 1e-16 - 1 - 2e-16 + 1e-16 + 1 + 2e-16 : 0,
      },
      {
        sizes: [2e-16, 1],
        during: usingTsImpl ? 0 - 2e-16 - 1 : -1,
        after: 0,
      },
    ];

    for (const { sizes, during, after } of cases) {
      const { writer, release } = gatedFpStream();
      const writes = sizes.map((v) => writer.write(v));
      await scheduler.wait(1); // let the TypeScript side start the sink
      strictEqual(writer.desiredSize, during);
      release();
      await Promise.all(writes);
      strictEqual(writer.desiredSize, after);
    }
  },
};

// A lone fractional chunk size: counted faithfully by TypeScript,
// truncated to zero by the C++ uint64 conversion.
export const fractionalSizeTruncation = {
  async test() {
    const { writer, release } = gatedFpStream();
    const write = writer.write(0.5);
    await scheduler.wait(1);
    strictEqual(writer.desiredSize, usingTsImpl ? -0.5 : 0);
    release();
    await write;
    strictEqual(writer.desiredSize, 0);
  },
};

// Invalid size() return values reject the write and error the stream;
// the error type diverges (C++ TypeError from the uint64 conversion with
// value-specific messages, TypeScript the spec's RangeError).
export const invalidSizeReturnRejects = {
  async test() {
    const cases = [
      { ret: NaN, cppMessage: /not an integer/ },
      { ret: -1, cppMessage: /negative/ },
      { ret: Infinity, cppMessage: /not an integer/ },
    ];
    for (const { ret, cppMessage } of cases) {
      const ws = new WritableStream(
        {},
        {
          size() {
            return ret;
          },
          highWaterMark: 5,
        }
      );
      const writer = ws.getWriter();
      const expected = usingTsImpl
        ? { name: 'RangeError', message: 'Invalid chunk size' }
        : { name: 'TypeError', message: cppMessage };
      await rejects(writer.write('x'), expected);
      // The bad size does not just doom the write; the stream errors.
      await rejects(writer.closed, expected);
    }
  },
};

// DIVERGENCE: while the stream is ERRORING (in-flight write outstanding,
// controller.error() already called) the spec reports desiredSize null;
// TypeScript and C++ with pedantic_wpt do that, while C++ otherwise still
// reports the queue accounting value. Once fully errored, everyone
// reports null.
export const desiredSizeWhileErroring = {
  async test() {
    let resolveWrite;
    let controller;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      write() {
        return new Promise((res) => (resolveWrite = res));
      },
    });
    const writer = ws.getWriter();
    await writer.ready;
    const write = writer.write('x');
    await scheduler.wait(1); // the write is in flight; erroring can begin
    controller.error(new Error('e'));
    strictEqual(writer.desiredSize, usingTsImpl || pedanticWpt ? null : 0);
    resolveWrite();
    await write;
    strictEqual(writer.desiredSize, null);
    await writer.closed.catch(() => {});
  },
};
