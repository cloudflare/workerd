// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entrancy edges: user callbacks (readable-side strategy size(),
// transformer hooks) re-entering the controller mid-operation. The two
// UAF regressions are migrated from transform-streams-test.js: the
// readable-side size() runs inside ReadableStreamDefaultController::
// enqueue(), and erroring the transform from there used to drop the last
// controller reference while its frame was still on the stack.

import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// DIVERGENCE (both UAF tests): with a read already PENDING, the
// TypeScript enqueue hands the chunk directly to the reader without
// queuing, so the readable-side size() never runs at all — the read
// fulfills with the chunk and no error ever fires. C++ measures the
// chunk first: size() runs, errors the transform from inside
// ReadableStreamDefaultController::enqueue(), and the read rejects.
// (Probed; the sequential shape — write settled before the first read —
// is parity: both sides reject the read.)

// controller.error() from inside the readableStrategy size() during an
// enqueue must not crash [C++ regression: calling error() there used to
// drop the controller refs mid-enqueue, a use-after-free].
export const sizeCallbackErrorDoesNotUAF = {
  async test() {
    let transformCtrl;
    let sizeCalls = 0;
    const ts = new TransformStream(
      {
        start(controller) {
          transformCtrl = controller;
        },
        transform(chunk, controller) {
          controller.enqueue(chunk);
        },
      },
      {},
      {
        size(_chunk) {
          sizeCalls++;
          // Calling error() here drops the ReadableStreamDefaultController refs.
          // Without the fix this is a use-after-free.
          transformCtrl.error(new Error('errored from size'));
          return 1;
        },
        highWaterMark: 1,
      }
    );

    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    // The write triggers transform -> enqueue -> size() -> error().
    // The key assertion: we reach this point without crashing (no UAF).
    const results = await Promise.allSettled([
      writer.write('hello'),
      reader.read(),
    ]);

    strictEqual(results[0].status, 'fulfilled');
    if (usingTsImpl) {
      // Direct handoff to the pending read: size() never consulted.
      strictEqual(sizeCalls, 0);
      strictEqual(results[1].status, 'fulfilled');
      strictEqual(results[1].value.value, 'hello');
    } else {
      strictEqual(sizeCalls, 1);
      strictEqual(results[1].status, 'rejected');
      ok(results[1].reason.message.includes('errored from size'));
    }
  },
};

// Same as above, but the size callback throws after calling error().
export const sizeCallbackErrorAndThrowDoesNotUAF = {
  async test() {
    let transformCtrl;
    const ts = new TransformStream(
      {
        start(controller) {
          transformCtrl = controller;
        },
        transform(chunk, controller) {
          controller.enqueue(chunk);
        },
      },
      {},
      {
        size(_chunk) {
          transformCtrl.error(new Error('errored from size'));
          throw new Error('size threw');
        },
        highWaterMark: 1,
      }
    );

    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    // The key assertion: we reach this point without crashing (no UAF).
    const results = await Promise.allSettled([
      writer.write('hello'),
      reader.read(),
    ]);

    strictEqual(results[0].status, 'fulfilled');
    if (usingTsImpl) {
      strictEqual(results[1].status, 'fulfilled');
      strictEqual(results[1].value.value, 'hello');
    } else {
      strictEqual(results[1].status, 'rejected');
    }
  },
};

// The sequential shape of the same scenario IS parity: with no pending
// read, the chunk must be queued, size() runs on both sides, and the
// error dooms subsequent reads.
export const sizeCallbackErrorSequential = {
  async test() {
    let transformCtrl;
    const ts = new TransformStream(
      {
        start(controller) {
          transformCtrl = controller;
        },
        transform(chunk, controller) {
          controller.enqueue(chunk);
        },
      },
      {},
      {
        size() {
          transformCtrl.error(new Error('errored from size'));
          return 1;
        },
        highWaterMark: 1,
      }
    );

    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    strictEqual(await writer.write('hello'), undefined);
    const results = await Promise.allSettled([reader.read(), reader.read()]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[0].reason.message, 'errored from size');
    strictEqual(results[1].status, 'rejected');
    strictEqual(results[1].reason.message, 'errored from size');
  },
};

// --- Reentrant operations inside the readable-side size() ---
//
// These mirror WPT transform-streams/reentrant-strategies.any.js. Most of
// that file's C++ expectedFailures are NOT reentrancy bugs: the WPT tests
// use readableStrategy { highWaterMark: Infinity }, which the C++
// constructor rejects outright (see hwmInfinityRejected in
// construction.js), so the scenarios never run. Re-probed with a finite
// high-water mark, the enqueue/terminate/cancel/write/sync-write/error
// reentrancy semantics are PARITY, including error-object identity.
// The genuinely divergent ops are read(), writer.close(), and
// writable.abort() inside size() — pinned further below.

async function drainToArray(readable) {
  const reader = readable.getReader();
  const items = [];
  for (;;) {
    const { value, done } = await reader.read();
    if (done) return items;
    items.push(value);
  }
}

// controller.enqueue() inside size() (parity): the nested enqueue lands
// first, so the chunks come out reversed.
export const enqueueInsideSize = {
  async test() {
    let controller;
    let calls = 0;
    const ts = new TransformStream(
      {
        start(c) {
          controller = c;
        },
      },
      undefined,
      {
        size() {
          if (++calls < 2) controller.enqueue('b');
          return 1;
        },
        highWaterMark: 10,
      }
    );
    const writer = ts.writable.getWriter();
    await Promise.all([writer.write('a'), writer.close()]);
    strictEqual(calls, 2);
    deepStrictEqual(await drainToArray(ts.readable), ['b', 'a']);
  },
};

// controller.terminate() inside size() (parity): the readable closes
// before the chunk becomes readable, so the chunk is unreadable and the
// stream drains empty; the write still succeeds.
export const terminateInsideSize = {
  async test() {
    let controller;
    const ts = new TransformStream(
      {
        start(c) {
          controller = c;
        },
      },
      undefined,
      {
        size() {
          controller.terminate();
          return 1;
        },
        highWaterMark: 10,
      }
    );
    const writer = ts.writable.getWriter();
    await writer.write('a');
    deepStrictEqual(await drainToArray(ts.readable), []);
  },
};

// readable.cancel() inside size() (parity): the write succeeds, the
// cancel promise fulfills, and the writer's closed rejects with the
// cancel reason — the very object, not a copy.
export const readableCancelInsideSize = {
  async test() {
    const reason = new Error('cancel-reason');
    let cancelPromise;
    const ts = new TransformStream({}, undefined, {
      size() {
        cancelPromise = ts.readable.cancel(reason);
        return 1;
      },
      highWaterMark: 10,
    });
    const writer = ts.writable.getWriter();
    await writer.write('a');
    let closedErr;
    await writer.closed.catch((e) => (closedErr = e));
    strictEqual(closedErr, reason);
    strictEqual(await cancelPromise, undefined);
  },
};

// writer.write() inside size() (parity): the outer write's size() issues
// a nested write whose own size() call is suppressed by the calls guard;
// the nested chunk is enqueued after the outer one.
export const writerWriteInsideSize = {
  async test() {
    let writer;
    let writePromise1;
    let calls = 0;
    const ts = new TransformStream({}, undefined, {
      size() {
        if (++calls < 2) writePromise1 = writer.write('a');
        return 1;
      },
      highWaterMark: 10,
    });
    writer = ts.writable.getWriter();
    await scheduler.wait(10);
    const writePromise2 = writer.write('b');
    strictEqual(calls, 1);
    await Promise.all([writePromise1, writePromise2, writer.close()]);
    strictEqual(calls, 2);
    deepStrictEqual(await drainToArray(ts.readable), ['b', 'a']);
  },
};

// writer.write() inside a size() triggered by controller.enqueue()
// (parity): the nested enqueue completes first, so the chunks come out
// in the opposite order, and both size() calls happen synchronously
// within the enqueue.
export const syncWriterWriteInsideSize = {
  async test() {
    let controller;
    let writer;
    let writePromise;
    let calls = 0;
    const ts = new TransformStream(
      {
        start(c) {
          controller = c;
        },
      },
      undefined,
      {
        size() {
          if (++calls < 2) writePromise = writer.write('a');
          return 1;
        },
        highWaterMark: 10,
      }
    );
    writer = ts.writable.getWriter();
    await scheduler.wait(10);
    controller.enqueue('b');
    strictEqual(calls, 2);
    await Promise.all([writePromise, writer.close()]);
    deepStrictEqual(await drainToArray(ts.readable), ['a', 'b']);
  },
};

// controller.error() from size() rejects the read with the SAME error
// object on both sides (parity; identity is what WPT's
// promise_rejects_exactly checks — the message-level shape is pinned by
// sizeCallbackErrorSequential above).
export const sizeCallbackErrorIdentity = {
  async test() {
    const err = new Error('identity-check');
    let controller;
    const ts = new TransformStream(
      {
        start(c) {
          controller = c;
        },
      },
      undefined,
      {
        size() {
          controller.error(err);
          return 1;
        },
        highWaterMark: 10,
      }
    );
    const writer = ts.writable.getWriter();
    await writer.write('a');
    let readErr;
    await ts.readable
      .getReader()
      .read()
      .catch((e) => (readErr = e));
    strictEqual(readErr, err);
  },
};

// reader.read() inside size() at highWaterMark 0. Parity: the enqueue
// that triggers size() sees the reentrant read; that read is fulfilled
// by the PENDING WRITE's chunk (not the enqueued one — see
// whatwg/streams#794), the write completes, and size() is not called
// again for the handed-off chunk. DIVERGENCE in the total size() count:
// C++ consults size() once more when the enqueued chunk is later pulled,
// TypeScript never does.
export const readInsideSize = {
  async test() {
    let controller;
    let readPromise;
    let calls = 0;
    let reader;
    const ts = new TransformStream(
      {
        start(c) {
          controller = c;
        },
      },
      undefined,
      {
        size() {
          readPromise = reader.read();
          ++calls;
          return 1;
        },
        highWaterMark: 0,
      }
    );
    reader = ts.readable.getReader();
    const writer = ts.writable.getWriter();
    let writeResolved = false;
    const writePromise = writer.write('b').then(() => {
      writeResolved = true;
    });
    await scheduler.wait(10);
    strictEqual(writeResolved, false);
    controller.enqueue('a');
    strictEqual(calls, 1);
    await scheduler.wait(10);
    strictEqual(writeResolved, true);
    const { value, done } = await readPromise;
    strictEqual(done, false);
    strictEqual(value, 'b');
    await writePromise;
    strictEqual(calls, usingTsImpl ? 1 : 2);
  },
};

// DIVERGENCE: writer.close() inside the size() triggered by
// controller.enqueue() (highWaterMark 1). TypeScript (spec): the
// enqueued chunk is still delivered, then the stream reads done. C++:
// the reentrant close wins immediately — the queued chunk is dropped and
// the first read is already done. The close promise fulfills either way.
export const writerCloseInsideSize = {
  async test() {
    let writer;
    let closePromise;
    let controller;
    const ts = new TransformStream(
      {
        start(c) {
          controller = c;
        },
      },
      undefined,
      {
        size() {
          closePromise = writer.close();
          return 1;
        },
        highWaterMark: 1,
      }
    );
    writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();
    await scheduler.wait(10);
    controller.enqueue('a');
    const r1 = await reader.read();
    if (usingTsImpl) {
      strictEqual(r1.done, false);
      strictEqual(r1.value, 'a');
      const r2 = await reader.read();
      strictEqual(r2.done, true);
    } else {
      strictEqual(r1.done, true);
    }
    strictEqual(await closePromise, undefined);
  },
};

// DIVERGENCE: writable.abort() inside the size() triggered by
// controller.enqueue() (highWaterMark 1). TypeScript (spec): the queued
// chunk is still delivered, then reads reject with the abort reason.
// C++: the reentrant abort wins immediately — the first read already
// rejects. The reason keeps identity and the abort promise fulfills on
// both sides.
export const writableAbortInsideSize = {
  async test() {
    const reason = new Error('abort-reason');
    let abortPromise;
    let controller;
    const ts = new TransformStream(
      {
        start(c) {
          controller = c;
        },
      },
      undefined,
      {
        size() {
          abortPromise = ts.writable.abort(reason);
          return 1;
        },
        highWaterMark: 1,
      }
    );
    const reader = ts.readable.getReader();
    await scheduler.wait(10);
    controller.enqueue('a');
    if (usingTsImpl) {
      const r1 = await reader.read();
      strictEqual(r1.done, false);
      strictEqual(r1.value, 'a');
    }
    let readErr;
    await reader.read().catch((e) => (readErr = e));
    strictEqual(readErr, reason);
    strictEqual(await abortPromise, undefined);
  },
};
