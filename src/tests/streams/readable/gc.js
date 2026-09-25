// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// GC interactions: pending machinery must keep the stream linkage alive
// when user references are dropped. Requires --expose-gc (set in all
// three cell configs).

import { strictEqual, ok, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

async function collectGarbage() {
  for (let i = 0; i < 3; i++) {
    gc();
    await scheduler.wait(5);
  }
}

// A controller whose stream was teed with both branches left unreachable.
function makeTeedAway(source = {}, highWaterMark = 4) {
  let controller;
  const rs = new ReadableStream(
    {
      ...source,
      start(c) {
        controller = c;
      },
    },
    new CountQueuingStrategy({ highWaterMark })
  );
  (() => {
    rs.tee();
  })();
  return controller;
}

// Enqueue n chunks tracked only weakly. A separate frame, so no chunk
// lingers in the caller's slots.
function enqueueTracked(controller, n) {
  const refs = [];
  for (let i = 0; i < n; i++) {
    const chunk = { i };
    refs.push(new WeakRef(chunk));
    controller.enqueue(chunk);
  }
  return refs;
}

function countAlive(refs) {
  let alive = 0;
  for (const ref of refs) {
    if (ref.deref() !== undefined) alive++;
  }
  return alive;
}

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

// Both tee branches collected while the source still holds the controller
// (parity). Nothing can read the queue again, so enqueue() accepts and
// drops each chunk, desiredSize stays at the high-water mark, and the
// controller's own state machine is unchanged: close() closes the source's
// stream, and a later enqueue() throws as ever.
export const teeBranchesCollected = {
  async test() {
    const controller = makeTeedAway();
    await collectGarbage();
    strictEqual(controller.desiredSize, 4);
    const refs = enqueueTracked(controller, 16);
    strictEqual(controller.desiredSize, 4);
    await collectGarbage();
    strictEqual(countAlive(refs), 0, 'dropped chunks must not be retained');
    controller.close();
    strictEqual(controller.desiredSize, 0);
    throws(() => controller.enqueue('late'), TypeError);
  },
};

// Chunks buffered for the branches are released once the branches are
// collected (parity).
export const teeBranchesCollectedReleaseBacklog = {
  async test() {
    const controller = makeTeedAway();
    const refs = enqueueTracked(controller, 8);
    strictEqual(controller.desiredSize, -4);
    await collectGarbage();
    strictEqual(controller.desiredSize, 4);
    await collectGarbage();
    strictEqual(countAlive(refs), 0, 'the backlog must be released');
  },
};

// One branch cancelled, the other collected (parity), observed in the same
// job as the gc(), before any finalization callback can run: the
// controller's own queries notice the collected consumer. Either branch may
// be the survivor. enqueue() drops and desiredSize stays at the high-water
// mark.
export const teeSurvivorBranchCollected = {
  async test() {
    for (const cancelFirst of [true, false]) {
      let controller;
      const rs = new ReadableStream(
        {
          start(c) {
            controller = c;
          },
        },
        new CountQueuingStrategy({ highWaterMark: 4 })
      );
      (() => {
        const [a, b] = rs.tee();
        // A lone branch's cancel promise pends under TypeScript (ledger #11).
        (cancelFirst ? a : b).cancel('bye');
      })();
      await scheduler.wait(1);
      gc();
      strictEqual(controller.desiredSize, 4, `cancelFirst=${cancelFirst}`);
      for (let i = 0; i < 16; i++) controller.enqueue(i);
      strictEqual(controller.desiredSize, 4, `cancelFirst=${cancelFirst}`);
    }
  },
};

// A pull source with both branches collected (ledger #20). TypeScript
// releases the source: pull() is never called again. C++ keeps pulling
// for consumers that no longer exist, so a source that enqueues on every
// pull runs until the stream closes.
export const teeBranchesCollectedPullStops = {
  async test() {
    let pulls = 0;
    const controller = makeTeedAway(
      {
        async pull(c) {
          pulls++;
          await scheduler.wait(1);
          // Bound the C++ side's loop so it does not outlive the test.
          if (pulls >= 200) c.close();
          else c.enqueue(pulls);
        },
      },
      1
    );
    await collectGarbage();
    strictEqual(controller.desiredSize, 1);
    const pullsAfterCollection = pulls;
    // An enqueue is what restarts the C++ loop once the consumers are gone.
    controller.enqueue('poke');
    await scheduler.wait(50);
    if (usingTsImpl) {
      strictEqual(pulls, pullsAfterCollection);
    } else {
      ok(pulls > pullsAfterCollection, `C++ keeps pulling (${pulls})`);
    }
  },
};
