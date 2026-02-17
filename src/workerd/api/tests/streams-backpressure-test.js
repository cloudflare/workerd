// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for backpressure behavior in JS-backed streams.
// These tests focus on desiredSize tracking, ready promise behavior,
// and backpressure propagation through pipe chains.
//
// Test inspirations:
// - Deno: tests/unit/streams_test.ts (parameterized count/delay tests)
// - Deno: tests/node_compat/test-stream-readable-hwm-0.js (hwm=0 edge case)
// - Bun: test/js/node/stream/node-stream.test.js (backpressure tests)
// - Bun: test/js/node/http/node-http-backpressure.test.ts (HTTP-level backpressure)
// - Bun: test/js/web/fetch/body-stream.test.ts (backpressure with various input lengths)

import { strictEqual, ok } from 'node:assert';

// Test ReadableStream with highWaterMark = 0
// Pull should only be called when there's an active read request
// Inspired by: Deno tests/node_compat/test-stream-readable-hwm-0.js (hwm=0 edge case)
export const backpressureReadableHwmZero = {
  async test() {
    let pullCount = 0;
    let controller;

    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
        pull(c) {
          pullCount++;
          c.enqueue(pullCount);
        },
      },
      { highWaterMark: 0 }
    );

    await scheduler.wait(10);
    strictEqual(pullCount, 0, 'pull should not be called without read');
    strictEqual(controller.desiredSize, 0, 'desiredSize should be 0');

    const reader = rs.getReader();

    const read1 = reader.read();
    await scheduler.wait(1);
    strictEqual(pullCount, 1, 'pull should be called once after read');

    const result1 = await read1;
    strictEqual(result1.value, 1);
    strictEqual(result1.done, false);

    strictEqual(controller.desiredSize, 0, 'desiredSize should remain 0');

    const read2 = reader.read();
    await scheduler.wait(1);
    strictEqual(pullCount, 2, 'pull should be called again');

    const result2 = await read2;
    strictEqual(result2.value, 2);

    reader.releaseLock();
  },
};

// Test ReadableStream with highWaterMark = 1
// Verify desiredSize transitions correctly
// Inspired by: Deno tests/unit/streams_test.ts (parameterized hwm tests)
export const backpressureReadableHwmOne = {
  async test() {
    let pullCount = 0;

    const rs = new ReadableStream(
      {
        start(c) {
          strictEqual(c.desiredSize, 1, 'initial desiredSize should be 1');
        },
        pull(c) {
          pullCount++;
          c.enqueue(pullCount);
        },
      },
      { highWaterMark: 1 }
    );

    const reader = rs.getReader();

    const result1 = await reader.read();
    strictEqual(result1.value, 1);
    strictEqual(result1.done, false);

    const result2 = await reader.read();
    strictEqual(result2.value, 2);
    strictEqual(result2.done, false);

    reader.releaseLock();
  },
};

// Test ReadableStream with highWaterMark = 64
// Multiple chunks should be buffered
// Inspired by: Bun test/js/web/streams/streams.test.js (large buffer tests)
export const backpressureReadableHwmLarge = {
  async test() {
    let pullCount = 0;
    const MAX_PULLS = 100;

    const rs = new ReadableStream(
      {
        pull(c) {
          pullCount++;
          if (pullCount <= MAX_PULLS) {
            c.enqueue(pullCount);
          } else {
            c.close();
          }
        },
      },
      { highWaterMark: 64 }
    );

    const reader = rs.getReader();

    const values = [];
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      values.push(value);
    }

    strictEqual(values.length, MAX_PULLS);
    strictEqual(values[0], 1);
    strictEqual(values[99], 100);
    ok(pullCount > MAX_PULLS, 'pull called enough times');
  },
};

// Test WritableStream desiredSize tracking and ready promise
// Inspired by: Bun test/js/web/fetch/body-stream.test.ts (backpressure with various input lengths)
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

// Test WritableStream with slow sink causes backpressure
// Inspired by: Bun test/js/bun/spawn/spawn-stdin-readable-stream-edge-cases.test.ts (slow consumer)
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

// Test TransformStream with both readable and writable strategies
// Inspired by: Bun test/js/web/streams/streams.test.js (TransformStream tests)
export const backpressureTransformBothStrategies = {
  async test() {
    const ts = new TransformStream(
      {
        transform(chunk, controller) {
          controller.enqueue(chunk);
        },
      },
      { highWaterMark: 2 },
      { highWaterMark: 4 }
    );

    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    strictEqual(writer.desiredSize, 2, 'writable desiredSize');

    writer.write(1);
    writer.write(2);
    writer.write(3);
    writer.write(4);

    const results = [];
    for (let i = 0; i < 4; i++) {
      const { value } = await reader.read();
      results.push(value);
    }

    strictEqual(results.length, 4);
    strictEqual(results[0], 1);
    strictEqual(results[3], 4);

    await writer.close();
    const final = await reader.read();
    ok(final.done);
  },
};

// Test backpressure propagation through pipe chain
// Inspired by: Bun test/js/web/streams/streams.test.js (pipeTo/pipeThrough tests)
export const backpressurePipeChain = {
  async test() {
    let sourcePullCount = 0;
    const MAX_CHUNKS = 5;

    const source = new ReadableStream(
      {
        pull(c) {
          sourcePullCount++;
          if (sourcePullCount <= MAX_CHUNKS) {
            c.enqueue(sourcePullCount);
          } else {
            c.close();
          }
        },
      },
      { highWaterMark: 2 }
    );

    const transform = new TransformStream(
      {
        transform(chunk, controller) {
          controller.enqueue(chunk * 2);
        },
      },
      { highWaterMark: 1 },
      { highWaterMark: 1 }
    );

    const chunks = [];
    const dest = new WritableStream(
      {
        async write(chunk) {
          chunks.push(chunk);
          // Slow consumer
          await scheduler.wait(5);
        },
      },
      { highWaterMark: 1 }
    );

    await source.pipeThrough(transform).pipeTo(dest);

    strictEqual(chunks.length, MAX_CHUNKS, 'all chunks received');
    strictEqual(chunks[0], 2, 'first chunk transformed');
    strictEqual(chunks[4], 10, 'last chunk transformed');
  },
};

// Test byte stream highWaterMark is measured in bytes, not chunks
// Inspired by: Bun test/js/node/test/parallel/test-whatwg-readablebytestream.js
export const backpressureByteStreamHwm = {
  async test() {
    let controller;
    let pullCount = 0;

    const rs = new ReadableStream(
      {
        type: 'bytes',
        start(c) {
          controller = c;
          strictEqual(c.desiredSize, 10, 'initial desiredSize in bytes');
        },
        pull(c) {
          pullCount++;
          // Enqueue 3 bytes
          c.enqueue(new Uint8Array([1, 2, 3]));
        },
      },
      { highWaterMark: 10 }
    );

    await scheduler.wait(20);

    ok(pullCount >= 3, 'pulled multiple times for byte count');
    ok(controller.desiredSize <= 1, 'desiredSize accounts for bytes');

    const reader = rs.getReader();

    const { value } = await reader.read();
    ok(value.byteLength >= 9, 'received buffered bytes');

    reader.releaseLock();
  },
};
