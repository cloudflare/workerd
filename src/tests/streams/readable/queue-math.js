// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Queue total-size arithmetic (the WPT floating-point-total-queue-size
// seeds). DIVERGENCE throughout: the spec requires the queue total to be
// tracked in double-precision floating point (TypeScript matches every
// WPT expectation); the C++ queue tracks integer-ish sizes — fractional
// contributions vanish and large totals saturate. Each C++ value below
// is pinned exactly as observed.

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function setup() {
  let controller;
  const rs = new ReadableStream(
    {
      start(c) {
        controller = c;
      },
    },
    {
      size(x) {
        return x;
      },
      highWaterMark: 0,
    }
  );
  return { reader: rs.getReader(), controller };
}

export const queueMathNearMaxSafeInteger = {
  async test() {
    const { reader, controller } = setup();
    controller.enqueue(2);
    strictEqual(controller.desiredSize, -2);
    controller.enqueue(Number.MAX_SAFE_INTEGER);
    strictEqual(
      controller.desiredSize,
      usingTsImpl ? 0 - Number.MAX_SAFE_INTEGER - 2 : -1
    );
    await reader.read();
    strictEqual(
      controller.desiredSize,
      usingTsImpl ? 0 - Number.MAX_SAFE_INTEGER - 2 + 2 : 1
    );
    await reader.read();
    strictEqual(controller.desiredSize, 0);
  },
};

// The two near-zero WPT shapes use FRACTIONAL chunk sizes. KNOWN DEFECT
// on the C++ side (same family as bad-strategies' invalid-size hang): a
// fractional size truncates to zero in the queue total, and a read() of
// such a chunk spins the isolate synchronously — so on C++ only the
// synchronous enqueue-time desiredSize values are asserted (truncation
// pinned exactly) and no read is attempted. TypeScript runs the full
// WPT double-math shapes.
export const queueMathNearZeroClamped = {
  async test() {
    const { reader, controller } = setup();
    controller.enqueue(1e-16);
    strictEqual(controller.desiredSize, usingTsImpl ? -1e-16 : 0);
    controller.enqueue(1);
    strictEqual(controller.desiredSize, usingTsImpl ? 0 - 1e-16 - 1 : -1);
    if (!usingTsImpl) return;
    await reader.read();
    // Spec: subtracting the first chunk leaves the double-math residue.
    strictEqual(controller.desiredSize, 0 - 1e-16 - 1 + 1e-16);
    await reader.read();
    strictEqual(controller.desiredSize, 0);
  },
};

export const queueMathNearZeroEndsZero = {
  async test() {
    const { reader, controller } = setup();
    controller.enqueue(2e-16);
    strictEqual(controller.desiredSize, usingTsImpl ? -2e-16 : 0);
    controller.enqueue(1);
    strictEqual(controller.desiredSize, usingTsImpl ? 0 - 2e-16 - 1 : -1);
    if (!usingTsImpl) return;
    await reader.read();
    strictEqual(controller.desiredSize, 0 - 2e-16 - 1 + 2e-16);
    await reader.read();
    strictEqual(controller.desiredSize, 0);
  },
};
