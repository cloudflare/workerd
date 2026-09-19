// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// GC interactions for BYOB machinery. The heavier collection-pressure
// regressions live in api/tests/streams-internal-read-buffer-gc-test.js
// and the autovuln suites; these are the suite-local liveness pins.
// Requires --expose-gc (set in all cell configs).

import { strictEqual, ok, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

async function collectGarbage() {
  for (let i = 0; i < 3; i++) {
    gc();
    await scheduler.wait(5);
  }
}

// A byte controller whose stream was teed with both branches left
// unreachable.
function makeTeedAway(source = {}, highWaterMark = 4) {
  let controller;
  const rs = new ReadableStream(
    {
      ...source,
      type: 'bytes',
      start(c) {
        controller = c;
      },
    },
    { highWaterMark }
  );
  (() => {
    rs.tee();
  })();
  return controller;
}

// Both tee branches collected while the source still holds the controller.
// A parity pin of the observable surface only: enqueue() accepts each
// chunk, desiredSize stays at the high-water mark, no byobRequest is
// minted, and close() then a late enqueue() behave as ever. Whether the
// dropped chunks are retained is checked in the readable suite's gc.js;
// a byte stream cannot express it, since enqueue() transfers the chunk's
// buffer and leaves nothing to hold a WeakRef to.
export const teeBranchesCollected = {
  async test() {
    const controller = makeTeedAway();
    await collectGarbage();
    strictEqual(controller.desiredSize, 4);
    for (let i = 0; i < 16; i++) {
      controller.enqueue(new Uint8Array(1024));
    }
    strictEqual(controller.desiredSize, 4);
    strictEqual(controller.byobRequest, null);
    controller.close();
    strictEqual(controller.desiredSize, 0);
    throws(() => controller.enqueue(new Uint8Array(1)), TypeError);
  },
};

// A pull source with both branches collected (ledger #26, the readable
// suite's #20). TypeScript releases the source: pull() is never called
// again. C++ keeps pulling for consumers that no longer exist, so a source
// that enqueues on every pull runs until the stream closes.
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
          else c.enqueue(new Uint8Array(1));
        },
      },
      1
    );
    await collectGarbage();
    strictEqual(controller.desiredSize, 1);
    const pullsAfterCollection = pulls;
    // An enqueue is what restarts the C++ loop once the consumers are gone.
    controller.enqueue(new Uint8Array(1));
    await scheduler.wait(50);
    if (usingTsImpl) {
      strictEqual(pulls, pullsAfterCollection);
    } else {
      ok(pulls > pullsAfterCollection, `C++ keeps pulling (${pulls})`);
    }
  },
};

// A pending BYOB read whose stream and reader references are dropped
// still completes when the controller responds.
export const pendingByobReadSurvivesGc = {
  async test() {
    let controller;
    let read;
    {
      let reader;
      {
        const rs = new ReadableStream({
          type: 'bytes',
          start(c) {
            controller = c;
          },
        });
        reader = rs.getReader({ mode: 'byob' });
      }
      await scheduler.wait(10);
      gc();
      read = reader.read(new Uint8Array(4));
      reader = undefined;
    }
    await scheduler.wait(10);
    gc();
    const req = controller.byobRequest;
    ok(req !== null);
    req.view[0] = 42;
    req.respond(1);
    controller = undefined;
    const { value, done } = await read;
    strictEqual(done, false);
    strictEqual(value.byteLength, 1);
    strictEqual(value[0], 42);
  },
};

// A byobRequest held across gc() with the stream reference dropped
// remains usable.
export const byobRequestSurvivesGc = {
  async test() {
    let req;
    let read;
    {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });
      const reader = rs.getReader({ mode: 'byob' });
      read = reader.read(new Uint8Array(2));
      await scheduler.wait(5);
      req = controller.byobRequest;
      ok(req !== null);
    }
    await scheduler.wait(10);
    gc();
    req.view[0] = 7;
    req.respond(1);
    const { value } = await read;
    strictEqual(value[0], 7);
  },
};
