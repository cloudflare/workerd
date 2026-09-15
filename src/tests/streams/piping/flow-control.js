// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Backpressure through pipes: the WPT flow-control seeds plus the
// pipeThrough().pipeTo() chain migrated from
// streams-backpressure-test.js.

import { strictEqual, ok } from 'node:assert';

// Backpressure propagates through a pipeThrough().pipeTo() chain with a
// slow consumer: every chunk arrives, transformed, in order (migrated
// from streams-backpressure-test.js backpressurePipeChain).
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
          await scheduler.wait(5); // slow consumer
        },
      },
      { highWaterMark: 1 }
    );
    await source.pipeThrough(transform).pipeTo(dest);
    strictEqual(chunks.length, MAX_CHUNKS);
    strictEqual(chunks[0], 2);
    strictEqual(chunks[4], 10);
  },
};

// A destination whose first write stalls: how far does the pipe keep
// reading? The source is FINITE (10 chunks, never closed) so both
// implementations stay bounded. With a source hwm of 1 BOTH
// implementations stop pulling promptly (read-ahead ≤ 3) — contrast
// with error-propagation's hwm-0 destination case, where C++ still
// writes an available chunk despite desiredSize 0.
export const pipeStopsPullingWhenDestStalls = {
  async test() {
    const MAX = 10;
    let pullCount = 0;
    let rc;
    const rs = new ReadableStream(
      {
        start(c) {
          rc = c;
        },
        pull(c) {
          if (pullCount < MAX) {
            pullCount++;
            c.enqueue(pullCount);
          }
        },
      },
      { highWaterMark: 1 }
    );
    let releaseWrite;
    const stuck = new Promise((resolve) => (releaseWrite = resolve));
    const wrote = [];
    const ws = new WritableStream(
      {
        write(chunk) {
          wrote.push(chunk);
          if (wrote.length === 1) return stuck; // first write stalls
          return undefined;
        },
      },
      { highWaterMark: 1 }
    );
    const pipeP = rs.pipeTo(ws);
    await scheduler.wait(30);
    const stalledPulls = pullCount;
    // Only the first sink write ran; later chunks (if any) sit queued
    // behind the stalled write.
    strictEqual(wrote.length, 1);
    ok(
      stalledPulls <= 3,
      `pipe read ahead too far while stalled (${stalledPulls} pulls)`
    );
    await scheduler.wait(30);
    strictEqual(pullCount, stalledPulls); // stable while stalled
    // Wind down deterministically: error the source FIRST so the pipe
    // terminates once the stalled write settles, then release it.
    const srcErr = new Error('wind-down');
    rc.error(srcErr);
    releaseWrite();
    const outcome = await Promise.race([
      pipeP.then(
        () => 'fulfilled',
        () => 'rejected'
      ),
      scheduler.wait(1000).then(() => 'pending'),
    ]);
    strictEqual(outcome, 'rejected');
  },
};
