// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Backpressure via highWaterMark on value streams: pull gating at
// hwm 0, desiredSize transitions at hwm 1, and buffered readahead at
// hwm 64 (migrated from streams-backpressure-test.js).

import { strictEqual, ok } from 'node:assert';

// hwm 0: pull runs only against an active read request.
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
    strictEqual(pullCount, 0);
    strictEqual(controller.desiredSize, 0);
    const reader = rs.getReader();
    const read1 = reader.read();
    await scheduler.wait(1);
    strictEqual(pullCount, 1);
    const result1 = await read1;
    strictEqual(result1.value, 1);
    strictEqual(result1.done, false);
    strictEqual(controller.desiredSize, 0);
    const read2 = reader.read();
    await scheduler.wait(1);
    strictEqual(pullCount, 2);
    const result2 = await read2;
    strictEqual(result2.value, 2);
    reader.releaseLock();
  },
};

// hwm 1: initial desiredSize is 1; sequential reads deliver in order.
export const backpressureReadableHwmOne = {
  async test() {
    let pullCount = 0;
    const rs = new ReadableStream(
      {
        start(c) {
          strictEqual(c.desiredSize, 1);
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

// hwm 64: the source is pulled ahead of demand and every buffered chunk
// arrives in order.
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
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      values.push(value);
    }
    strictEqual(values.length, MAX_PULLS);
    strictEqual(values[0], 1);
    strictEqual(values[99], 100);
    ok(pullCount > MAX_PULLS, `pull ran ahead of demand (${pullCount})`);
  },
};
