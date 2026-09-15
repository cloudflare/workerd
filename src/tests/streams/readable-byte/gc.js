// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// GC interactions for BYOB machinery. The heavier collection-pressure
// regressions live in api/tests/streams-internal-read-buffer-gc-test.js
// and the autovuln suites; these are the suite-local liveness pins.
// Requires --expose-gc (set in all cell configs).

import { strictEqual, ok } from 'node:assert';

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
