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
