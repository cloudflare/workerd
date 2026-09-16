// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// GC interactions: pending machinery must keep the stream linkage alive
// when user references are dropped. Requires --expose-gc (set in all
// three cell configs).

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// A pending read with the stream and reader references dropped still
// completes when the (still-referenced) controller enqueues (the value
// half of streams-js-test.js readableStreamReferencesHold).
export const pendingReadSurvivesGc = {
  async test() {
    let controller;
    let read;
    {
      let reader;
      {
        const rs = new ReadableStream({
          start(c) {
            controller = c;
          },
        });
        reader = rs.getReader();
      }
      await scheduler.wait(10);
      gc();
      read = reader.read();
      reader = undefined;
    }
    await scheduler.wait(10);
    gc();
    controller.enqueue('hello');
    controller = undefined;
    const { value, done } = await read;
    ok(!done);
    strictEqual(value, 'hello');
  },
};

// An async iteration in flight survives gc() of the original stream
// reference (migrated from streams-js-test.js asyncIteratorGc, value
// shape).
export const asyncIterationSurvivesGc = {
  async test() {
    let it;
    {
      const rs = new ReadableStream({
        start(c) {
          c.enqueue('a');
          c.enqueue('b');
          c.close();
        },
      });
      it = rs[Symbol.asyncIterator]();
    }
    await scheduler.wait(10);
    gc();
    strictEqual((await it.next()).value, 'a');
    gc();
    strictEqual((await it.next()).value, 'b');
    strictEqual((await it.next()).done, true);
  },
};

async function createPendingReadableStreamFromRefs() {
  const refs = [];
  for (let i = 0; i < 8; i++) {
    let nextCalled = false;
    const { promise: pending, resolve } = Promise.withResolvers();
    const iterator = {
      resolve,
      next() {
        nextCalled = true;
        return pending;
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };
    const stream = ReadableStream.from(iterator);
    refs.push(new WeakRef(iterator), new WeakRef(stream));

    const reader = stream.getReader();
    reader.read().catch(() => {});
    await scheduler.wait(0);
    ok(nextCalled, 'the unresolved pull was not started');
    reader.releaseLock();
  }
  return refs;
}

export const readableStreamFromPendingPromiseCollects = {
  async test() {
    const refs = await createPendingReadableStreamFromRefs();
    strictEqual(refs.length, 16);

    for (let i = 0; i < 4; i++) {
      await scheduler.wait(0);
      gc();
    }

    let alive = 0;
    for (const ref of refs) {
      if (ref.deref() !== undefined) alive++;
    }
    ok(
      alive <= 2,
      `expected pending ReadableStream.from cycles to be collected, ` +
        `${alive} of ${refs.length} objects still alive`
    );
  },
};

// A controller held while its stream is dropped (ledger #19). Per spec the
// controller's [[stream]] slot keeps the stream alive, so enqueues keep
// counting against the high-water mark: TypeScript, where the controller
// strongly references the stream. Under C++ the controller does not keep
// its stream alive: once the stream is collected its consumer is gone from
// the controller's queue, enqueue() drops the chunk without throwing and
// desiredSize stays at the high-water mark — a producer holding only the
// controller never sees backpressure.
export const controllerOnlyHeldStreamLiveness = {
  async test() {
    let controller;
    const make = () => {
      new ReadableStream(
        {
          start(c) {
            controller = c;
          },
        },
        new ByteLengthQueuingStrategy({ highWaterMark: 4 })
      );
    };
    make();
    await scheduler.wait(10);
    gc();
    await scheduler.wait(10);
    gc();
    strictEqual(controller.desiredSize, 4);
    controller.enqueue(new Uint8Array(2));
    controller.enqueue(new Uint8Array(2));
    gc();
    await scheduler.wait(10);
    strictEqual(controller.desiredSize, usingTsImpl ? 0 : 4);
  },
};
