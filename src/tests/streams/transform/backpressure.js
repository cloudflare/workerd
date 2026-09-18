// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Backpressure through the transform: writable-side desiredSize
// accounting against the readable-side queue, and dual-strategy
// construction. Migrated from transform-streams-test.js and
// streams-backpressure-test.js. Runs with
// fixup_transform_stream_backpressure pinned in the C++ cells (the
// legacy-backpressure cell guards the original buggy accounting).

import { strictEqual, ok, rejects, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Tracks a write's settlement without awaiting it: a regression here parks
// the write, and an await would hang the cell.
function trackWrite(writer, chunk) {
  const state = { value: 'pending' };
  writer.write(chunk).then(
    () => (state.value = 'fulfilled'),
    () => (state.value = 'rejected')
  );
  return state;
}

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

// The default readable strategy is highWaterMark 0: the controller
// reports desiredSize 0 at start in both implementations. (The
// equivalent WPT strategies.any case is disabled for the C++ run; the
// direct observation is parity.)
export const defaultReadableHwmZero = {
  test() {
    let ctrl;
    new TransformStream({
      start(c) {
        ctrl = c;
      },
    });
    strictEqual(ctrl.desiredSize, 0);
  },
};

// With fixup-transform-stream-backpressure pinned (and hard-coded in
// TypeScript), an enqueue that crosses the readable high-water mark
// holds the NEXT write pending until a read drains the queue.
//
// DIVERGENCE, only partially pinnable: under TypeScript the full spec
// flow is deterministic and asserted below. Under C++ the dual-strategy
// release path is RACY — depending on how the transform pump interleaves
// with the backpressure latch, the second write either completes (and
// the chunk flows) or latches pending forever. That nondeterminism is
// exactly why WPT transform-streams/backpressure.any.js is disabled for
// the C++ run ("A hanging Promise was canceled"). Only the
// race-independent prefix (first write fulfilled, first chunk readable)
// is asserted for C++; the racy tail is documented in AGENTS.md rather
// than pinned.
export const backpressureAppliedAtReadableHwm = {
  async test() {
    const ts = new TransformStream(
      {
        transform(chunk, c) {
          c.enqueue(chunk);
        },
      },
      { highWaterMark: 4 },
      { highWaterMark: 1 }
    );
    const writer = ts.writable.getWriter();

    let w1state = 'pending';
    let w2state = 'pending';
    const w1 = writer.write('a').then(() => (w1state = 'fulfilled'));
    const w2 = writer.write('b').then(() => (w2state = 'fulfilled'));
    await scheduler.wait(10);

    // The first chunk is transformed and enqueued on both sides.
    strictEqual(w1state, 'fulfilled');

    if (usingTsImpl) {
      // Backpressure holds the second write until the drain (spec).
      strictEqual(w2state, 'pending');
      const reader = ts.readable.getReader();
      const r1 = await reader.read();
      strictEqual(r1.value, 'a');
      await w2;
      strictEqual(w2state, 'fulfilled');
      const r2 = await reader.read();
      strictEqual(r2.value, 'b');
      await Promise.all([w1, w2]);
    } else {
      // C++: only the race-independent prefix. The first chunk is
      // always deliverable; w2's fate is nondeterministic (see above).
      const reader = ts.readable.getReader();
      const r1 = await reader.read();
      strictEqual(r1.value, 'a');
      await w1;
    }
  },
};

// A write parked on backpressure is released by a read's pull. An enqueue on
// a stored controller in the same turn re-asserts backpressure before the
// write's reaction runs; the write transforms anyway (spec
// TransformStreamDefaultSinkWriteAlgorithm waits once). Parity. Re-checking
// the flag after the wait would park the write until the next read, and a
// producer that awaits each write before reading again would deadlock.
export const writeReleasedByReadSurvivesSameTurnEnqueue = {
  async test() {
    let ctl;
    const ts = new TransformStream({
      start(c) {
        ctl = c;
      },
      transform(chunk, c) {
        c.enqueue(chunk.toUpperCase());
      },
    });
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    const write = trackWrite(writer, 'a');
    await scheduler.wait(1);
    strictEqual(write.value, 'pending');

    const read = reader.read();
    ctl.enqueue('x');
    await scheduler.wait(10);
    strictEqual(write.value, 'fulfilled');
    strictEqual((await read).value, 'x');
    strictEqual((await reader.read()).value, 'A');
  },
};

// Same release through a dequeue at readable hwm 1 instead of a pending
// read. DIVERGENCE (ledger #14): C++ does not pull when a read leaves the
// queue with room (readable ledger #5), so the write stays parked until a
// read finds the queue empty; TS pulls on the dequeue and the write
// transforms although the same-turn enqueue re-asserted backpressure (spec).
export const writeReleasedByDequeueSurvivesSameTurnEnqueue = {
  async test() {
    let ctl;
    const ts = new TransformStream(
      {
        start(c) {
          ctl = c;
        },
        transform(chunk, c) {
          c.enqueue(chunk.toUpperCase());
        },
      },
      undefined,
      { highWaterMark: 1 }
    );
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    await writer.write('a');
    const write = trackWrite(writer, 'b');
    await scheduler.wait(1);
    strictEqual(write.value, 'pending');

    const read = reader.read();
    ctl.enqueue('x');
    strictEqual((await read).value, 'A');
    await scheduler.wait(10);
    strictEqual(write.value, usingTsImpl ? 'fulfilled' : 'pending');
    strictEqual((await reader.read()).value, 'x');
    strictEqual((await reader.read()).value, 'B');
    await scheduler.wait(1);
    strictEqual(write.value, 'fulfilled');
  },
};

// The release's turn also errors the controller: the released write finds
// the writable erroring and rejects without transforming. DIVERGENCE
// (ledger #15): TS rejects with the writable's stored error (spec); C++ has
// already dropped its writable reference and cleared the algorithms, so the
// write takes the identity path into the errored readable and rejects with
// that enqueue's TypeError.
export const writeReleasedByReadThenErroredSameTurn = {
  async test() {
    let ctl;
    const transformed = [];
    const ts = new TransformStream({
      start(c) {
        ctl = c;
      },
      transform(chunk) {
        transformed.push(chunk);
      },
    });
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    const write = writer.write('a');
    await scheduler.wait(1);

    const read = reader.read();
    ctl.enqueue('x');
    const reason = new Error('boom');
    ctl.error(reason);
    strictEqual((await read).value, 'x');
    await rejects(
      write,
      usingTsImpl
        ? (e) => e === reason
        : {
            name: 'TypeError',
            message:
              'The readable side of this TransformStream is no longer readable.',
          }
    );
    deepStrictEqual(transformed, []);
  },
};
