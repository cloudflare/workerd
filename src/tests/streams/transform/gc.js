// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The transform machinery keeps the controller/writable/readable linkage
// alive when the user drops the TransformStream itself between turns;
// gc() must not sever a write→read handoff in flight. Requires
// --expose-gc (set in both cell configs).

import { strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

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

async function collectGarbage() {
  for (let i = 0; i < 3; i++) {
    gc();
    await scheduler.wait(5);
  }
}

// A TransformStream whose transformer object is reachable only through
// the stream, plus a WeakRef to that transformer.
function makeTransform() {
  let controller;
  const transformer = {
    start(c) {
      controller = c;
    },
    transform(chunk, c) {
      c.enqueue(chunk);
    },
    flush() {},
    cancel() {},
  };
  const ts = new TransformStream(transformer);
  return { ts, controller, ref: new WeakRef(transformer) };
}

// Once the stream is closed, aborted, cancelled or terminated, the spec
// clears the transformer's algorithms (ClearAlgorithms), so the
// transformer is collectable while the TransformStream (and its
// controller) is still held. TypeScript and Node collect it after every
// ending; C++ keeps it after close (ledger #18).
export const transformerCollectedAfterFinish = {
  async test() {
    const endings = {
      async close(ts) {
        const writer = ts.writable.getWriter();
        const reader = ts.readable.getReader();
        const write = writer.write('x');
        strictEqual((await reader.read()).value, 'x');
        await write;
        await writer.close();
        strictEqual((await reader.read()).done, true);
      },
      async abort(ts) {
        await ts.writable.abort(new Error('abort')).catch(() => {});
      },
      async cancel(ts) {
        await ts.readable.cancel(new Error('cancel'));
      },
      async terminate(ts, controller) {
        controller.terminate();
        await ts.writable.getWriter().closed.catch(() => {});
      },
      async error(ts, controller) {
        controller.error(new Error('error'));
        await ts.writable.getWriter().closed.catch(() => {});
      },
    };
    const results = {};
    for (const [name, end] of Object.entries(endings)) {
      const { ts, controller, ref } = makeTransform();
      await end(ts, controller);
      await collectGarbage();
      results[name] = ref.deref() === undefined;
      // Keep the stream itself alive across the collection.
      strictEqual(typeof ts.readable, 'object');
    }
    if (usingTsImpl) {
      deepStrictEqual(results, {
        close: true,
        abort: true,
        cancel: true,
        terminate: true,
        error: true,
      });
    } else {
      // DIVERGENCE (ledger #18): C++ keeps the transformer after close.
      deepStrictEqual(results, {
        close: false,
        abort: true,
        cancel: true,
        terminate: true,
        error: true,
      });
    }
  },
};
