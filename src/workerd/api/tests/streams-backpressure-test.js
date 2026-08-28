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
