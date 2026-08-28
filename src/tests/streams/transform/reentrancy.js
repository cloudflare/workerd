// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entrancy edges: user callbacks (readable-side strategy size(),
// transformer hooks) re-entering the controller mid-operation. The two
// UAF regressions are migrated from transform-streams-test.js: the
// readable-side size() runs inside ReadableStreamDefaultController::
// enqueue(), and erroring the transform from there used to drop the last
// controller reference while its frame was still on the stack.

import { strictEqual, ok } from 'node:assert';
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
