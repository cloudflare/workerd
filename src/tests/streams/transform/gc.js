// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The transform machinery keeps the controller/writable/readable linkage
// alive when the user drops the TransformStream itself between turns;
// gc() must not sever a write→read handoff in flight. Requires
// --expose-gc (set in both cell configs).

import { strictEqual } from 'node:assert';

export const transformStreamGc = {
  async test() {
    let controller;
    let writer;
    let reader;

    {
      const ts = new TransformStream({
        start(c) {
          controller = c;
        },
        transform(chunk, c) {
          c.enqueue(chunk);
        },
      });
      writer = ts.writable.getWriter();
      reader = ts.readable.getReader();
    }

    await scheduler.wait(10);
    gc();

    const write = writer.write('x');

    await scheduler.wait(10);
    gc();

    const r = await reader.read();
    strictEqual(r.value, 'x');
    await write;
    strictEqual(controller.desiredSize, 0);
  },
};
