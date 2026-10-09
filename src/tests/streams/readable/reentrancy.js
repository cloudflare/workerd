// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Reentrant controller/stream operations inside the readable strategy's
// size() callback (WPT readable-streams/reentrant-strategies seeds; as
// with the transform suite, most scenarios are parity once run at a
// finite high-water mark — the WPT originals' Infinity is rejected by
// the C++ constructor, see construction.js highWaterMarkValidated).
// The size()-errors-the-stream shapes live in bad-strategies.js.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { drainToArray } from 'helpers';

// enqueue() inside size() (parity): the nested enqueue lands first, so
// the chunks come out reversed; size runs once per enqueue.
export const enqueueInsideSize = {
  async test() {
    let controller;
    let calls = 0;
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      {
        size() {
          if (++calls < 2) controller.enqueue('b');
          return 1;
        },
        highWaterMark: 10,
      }
    );
    controller.enqueue('a');
    controller.close();
    strictEqual(calls, 2);
    deepStrictEqual(await drainToArray(rs), ['b', 'a']);
  },
};

// close() inside size() (parity): the stream closes before the chunk
// becomes readable — the chunk is unreadable and reads are done.
export const closeInsideSize = {
  async test() {
    let controller;
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      {
        size() {
          controller.close();
          return 1;
        },
        highWaterMark: 10,
      }
    );
    controller.enqueue('a');
    const r = await rs.getReader().read();
    strictEqual(r.done, true);
  },
};

// stream.cancel() inside size() (parity): the cancel hook runs with the
// reason, the enqueue completes, and reads are done.
export const cancelInsideSize = {
  async test() {
    let controller;
    let cancelReason = 'not-called';
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
        cancel(r) {
          cancelReason = r;
        },
      },
      {
        size() {
          rs.cancel('from-size');
          return 1;
        },
        highWaterMark: 10,
      }
    );
    controller.enqueue('a');
    strictEqual(cancelReason, 'from-size');
    const r = await rs.getReader().read();
    strictEqual(r.done, true);
  },
};

// DIVERGENCE: reader.read() inside the size() triggered by an enqueue.
// The reentrant read is registered after the spec's pending-read check,
// so under TypeScript (spec, the WPT expectation) the in-flight chunk
// still goes to the QUEUE and the reentrant read is fulfilled by the
// NEXT enqueue, which bypasses the queue. C++ hands the in-flight chunk
// to the reentrant read directly, so deliveries are swapped.
export const readInsideSize = {
  async test() {
    let controller;
    let innerRead;
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      {
        size() {
          // Guarded: only the first enqueue plants the reentrant read.
          // (Under C++ the reentrant read is fed directly, so an
          // unguarded size() would capture every later chunk too.)
          innerRead ??= reader.read();
          return 1;
        },
        highWaterMark: 10,
      }
    );
    const reader = rs.getReader();
    await scheduler.wait(5);
    controller.enqueue('a');
    controller.enqueue('b');
    const inner = await innerRead;
    const next = await reader.read();
    if (usingTsImpl) {
      strictEqual(inner.value, 'b');
      strictEqual(next.value, 'a');
    } else {
      strictEqual(inner.value, 'a');
      strictEqual(next.value, 'b');
    }
  },
};

// A stream whose size() closes it when it meets `closeOn`, and the
// source's cancel calls.
function closingInSize(closeOn) {
  let controller;
  const cancels = [];
  const rs = new ReadableStream(
    {
      start(c) {
        controller = c;
      },
      cancel(reason) {
        cancels.push(reason);
      },
    },
    {
      size(chunk) {
        if (chunk === closeOn) controller.close();
        return 1;
      },
      highWaterMark: 10,
    }
  );
  return { rs, controller: () => controller, cancels };
}

async function readRest(reader) {
  const items = [];
  for (;;) {
    const { value, done } = await reader.read();
    if (done) return items;
    items.push(value);
  }
}

const PENDING = Symbol('pending');
async function settledValue(promise) {
  return Promise.race([promise, scheduler.wait(50).then(() => PENDING)]);
}

// DIVERGENCE (ledger #30): close() inside size() with a chunk already
// queued. The close is only requested, so the in-flight chunk is still
// readable under TypeScript (spec; Node agrees). C++ drops it.
export const closeInsideSizeWithQueuedChunk = {
  async test() {
    const { rs, controller } = closingInSize('y');
    controller().enqueue('x');
    controller().enqueue('y');
    deepStrictEqual(await drainToArray(rs), usingTsImpl ? ['x', 'y'] : ['x']);
  },
};

// close() inside size() on a teed stream, one branch drained and closed
// by the close (parity): the in-flight chunk is dropped for every branch,
// so the branches see the same chunks, and the other branch's cancel
// settles at once with undefined, without cancelling the source (the
// source has closed; Node agrees for the cancel).
export const closeInsideSizeTeeDrainedBranch = {
  async test() {
    for (const cancelSecond of [false, true]) {
      const { rs, controller, cancels } = closingInSize('y');
      const [b1, b2] = rs.tee();
      const r1 = b1.getReader();
      const r2 = b2.getReader();
      controller().enqueue('x');
      strictEqual((await r1.read()).value, 'x');
      controller().enqueue('y');
      strictEqual(await settledValue(r1.closed), undefined);
      if (cancelSecond) {
        strictEqual(await settledValue(r2.cancel('bye')), undefined);
      } else {
        deepStrictEqual(await readRest(r1), []);
        deepStrictEqual(await readRest(r2), ['x']);
      }
      deepStrictEqual(cancels, []);
    }
  },
};

// DIVERGENCE (ledger #30): close() inside size() on a teed stream whose
// branches are both behind. TypeScript delivers the in-flight chunk to
// both, as with no tee (close requested, the chunk still readable). C++
// drops it for both.
export const closeInsideSizeTeeBranchesBehind = {
  async test() {
    const { rs, controller } = closingInSize('y');
    const [b1, b2] = rs.tee();
    controller().enqueue('x');
    controller().enqueue('y');
    const expected = usingTsImpl ? ['x', 'y'] : ['x'];
    deepStrictEqual(await drainToArray(b1), expected);
    deepStrictEqual(await drainToArray(b2), expected);
  },
};
