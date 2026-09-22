// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The stream machinery keeps a pending write alive even when the user
// drops every reachable reference between turns; gc() must not sever the
// controller/writer/stream linkage. Requires --expose-gc (set in both
// cell configs). Migrated from streams-js-test.js.

import { strictEqual } from 'node:assert';

export const writableStreamGc = {
  async test() {
    let controller;
    let writer;
    let write;

    {
      const ws = new WritableStream({
        start(c) {
          controller = c;
        },
      });
      writer = ws.getWriter();
    }

    await scheduler.wait(10);
    gc();

    {
      write = writer.write(1);
      writer = undefined;
    }

    await scheduler.wait(10);
    gc();

    {
      await write;
      strictEqual(controller.signal.aborted, false);
    }
  },
};

// A bare WritableStream survives a gc() sweep without crashing the GC
// tracing path (migrated from streams-test.js).
export const writableStreamGcTraceFinishes = {
  test() {
    const _ws = new WritableStream();
    gc();
  },
};
