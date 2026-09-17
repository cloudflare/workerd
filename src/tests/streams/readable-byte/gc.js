// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// GC interactions for BYOB machinery. The heavier collection-pressure
// regressions live in api/tests/streams-internal-read-buffer-gc-test.js
// and the autovuln suites; these are the suite-local liveness pins.
// Requires --expose-gc (set in all cell configs).

import { strictEqual, ok, throws } from 'node:assert';

// Both tee branches collected while the source still holds the controller
// (parity; the value-stream shape, with the retention checks, is in the
// readable suite's gc.js). enqueue() accepts and drops each chunk,
// desiredSize stays at the high-water mark, no byobRequest is minted, and
// close() then a late enqueue() behave as ever.
export const teeBranchesCollected = {
  async test() {
    let controller;
    const rs = new ReadableStream(
      {
        type: 'bytes',
        start(c) {
          controller = c;
        },
      },
      { highWaterMark: 4 }
    );
    (() => {
      rs.tee();
    })();
    for (let i = 0; i < 3; i++) {
      gc();
      await scheduler.wait(5);
    }
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
