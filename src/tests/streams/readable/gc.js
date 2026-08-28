// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// GC interactions: pending machinery must keep the stream linkage alive
// when user references are dropped. Requires --expose-gc (set in all
// three cell configs).

import { strictEqual, ok } from 'node:assert';

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
