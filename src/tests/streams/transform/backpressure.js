// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Backpressure through the transform: writable-side desiredSize
// accounting against the readable-side queue, and dual-strategy
// construction. Migrated from transform-streams-test.js and
// streams-backpressure-test.js. Runs with
// fixup_transform_stream_backpressure pinned in the C++ cells (the
// legacy-backpressure cell guards the original buggy accounting).

import { strictEqual, ok } from 'node:assert';

// The transformer sees the readable-side desiredSize drain as it
// enqueues; the writable side recovers once the queue is consumed... or
// rather, once the transform completes.
export const writeBackpressure = {
  async test() {
    let expectedReadSize = 2;
    const transform = new TransformStream(
      {
        transform(chunk, controller) {
          strictEqual(controller.desiredSize, expectedReadSize--);
          controller.enqueue(chunk);
        },
      },
      { highWaterMark: 2 },
      { highWaterMark: 2 }
    );

    const writer = transform.writable.getWriter();
    strictEqual(writer.desiredSize, 2);

    const promises = [writer.write('hello'), writer.write('there')];

    strictEqual(writer.desiredSize, 0);

    await Promise.allSettled(promises);

    strictEqual(writer.desiredSize, 2);
  },
};

// Distinct writable and readable strategies apply to their own sides.
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
